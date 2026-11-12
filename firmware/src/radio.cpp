#include "radio.h"
#include "audio.h"
#include "rss_feed.h"
#include "rss_cache.h"
#include "sdcard.h"
#include "radio_dsp.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>
#include <memory>
#include <atomic>
#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#define DR_MP3_NO_SIMD
#include "dr_mp3.h"

extern const uint8_t radioCaStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t radioCaEnd[] asm("_binary_x509_crt_bundle_end");
namespace {
constexpr uint32_t RATE=16000,CAP=RATE*8,PREBUFFER=RATE*3;
SemaphoreHandle_t mux=nullptr;
struct Guard {Guard(){xSemaphoreTake(mux,portMAX_DELAY);}~Guard(){xSemaphoreGive(mux);}};
Preferences prefs;
std::atomic<bool> initialized{false};
bool enabled=false,want=false,refresh=false,eof=false,buffering=true,decoding=false;
uint32_t generation=1,head=0,tail=0,fill=0,played=0,baseSec=0,underruns=0,audioStart=0;
int selected=-1,bitrate=0;
int16_t* ring=nullptr;
String feedUrl,error,state="idle",show;
std::vector<radiofeed::Episode> episodes;
PlayerInfo info={};

bool urlOK(const String& u){return radiofeed::urlOK(std::string(u.c_str()))&&u.indexOf('\n')<0&&u.indexOf('\r')<0&&u.indexOf(' ')<0;}
void copyText(char* out,size_t size,const char* text){auto s=radiofeed::clipped(text?std::string(text):std::string(),size-1);memcpy(out,s.data(),s.size());out[s.size()]=0;}
bool alive(uint32_t g){Guard l;return enabled&&generation==g;}
void fail(uint32_t g,const String& why){Guard l;if(g==generation){error=why;state="error";want=false;}}
void clearBuffer(){head=tail=fill=played=0;buffering=true;eof=false;}

// Metadata only, on an already-mounted SD card. Never format storage here.
bool cacheStorageReady=false,cachePending=true;
bool cacheDirty=false; // owned by the network/decoder worker
fs::FS* cacheFs=nullptr;
uint32_t cacheRetryAt=0;
uint64_t cacheSequence=0;
String cacheNote;
constexpr const char* CACHE_SLOTS[2]={"/agentpet/rss-a.json","/agentpet/rss-b.json"};
constexpr const char* CACHE_TEMP="/agentpet/rss.tmp";
struct CacheAllocator : ArduinoJson::Allocator {
 void* allocate(size_t n) override{return heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
 void deallocate(void* p) override{free(p);}
 void* reallocate(void* p,size_t n) override{return heap_caps_realloc(p,n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);}
};
bool cacheLoad(fs::FS& fs,uint32_t g,const String& url){
 std::vector<radiofeed::Episode> best;String cachedShow;uint64_t bestSeq=0;
 for(const char* path:CACHE_SLOTS){
  File file=fs.open(path,FILE_READ);if(!file||file.size()>160*1024){if(file)file.close();continue;}
  CacheAllocator allocator;JsonDocument doc(&allocator);auto err=deserializeJson(doc,file);file.close();if(err)continue;
  uint64_t seq=doc["seq"] | (uint64_t)0;
  cacheSequence=std::max(cacheSequence,seq); // worker owns the disk sequence
  if((doc["version"] | 0)!=1||String(doc["feed"] | "")!=url||seq<bestSeq)continue;
  auto items=doc["items"].as<JsonArray>();if(items.isNull()||items.size()>radiofeed::CACHE_LIMIT)continue;
  std::vector<radiofeed::Episode> valid;
  for(JsonObject item:items){radiofeed::Episode e;e.url=item["url"] | "";e.title=item["title"] | "";e.duration=item["dur"] | 0u;
   if(!radiofeed::urlOK(e.url)||e.title.size()>191||e.duration>7*86400u){valid.clear();break;}
   valid.push_back(e);
  }
  if(!valid.empty()){best=radiofeed::mergeCache(valid,{});bestSeq=seq;cachedShow=doc["show"] | "";}
 }
 if(best.empty())return false;
 Guard l;if(g!=generation||url!=feedUrl)return false;
 // Inserting a card during playback must not replace the current selection.
 if(!episodes.empty())return radiofeed::sameCache(episodes,best)&&show==cachedShow;
 episodes=std::move(best);show=cachedShow;selected=0;refresh=false;want=false;state="idle";error="";
 return true;
}
bool cacheSave(fs::FS& fs,uint32_t g,const String& url){
 uint64_t nextSequence=cacheSequence+1;
 CacheAllocator allocator;JsonDocument doc(&allocator);doc["version"]=1;doc["feed"]=url;doc["seq"]=nextSequence;
 {Guard l;if(g!=generation||url!=feedUrl||episodes.empty())return false;doc["show"]=show;
  JsonArray items=doc["items"].to<JsonArray>();for(const auto& e:episodes){JsonObject item=items.add<JsonObject>();item["title"]=e.title;item["url"]=e.url;item["dur"]=e.duration;}
 }
 if(doc.overflowed()||measureJson(doc)>160*1024){Guard l;cacheNote="metadata cache too large";return false;}
 String data;serializeJson(doc,data);
 if(!fs.exists("/agentpet")&&!fs.mkdir("/agentpet")){Guard l;cacheNote="SD cache directory unavailable";return false;}
 File tmp=fs.open(CACHE_TEMP,FILE_WRITE);
 bool ok=tmp&&tmp.write((const uint8_t*)data.c_str(),data.length())==data.length();if(tmp){tmp.flush();tmp.close();}
 const char* destination=CACHE_SLOTS[nextSequence%2];
 if(ok){if(fs.exists(destination))ok=fs.remove(destination);if(ok)ok=fs.rename(CACHE_TEMP,destination);}
 // The other slot remains valid if power is lost before this replacement.
 if(!ok)fs.remove(CACHE_TEMP);
 if(ok)cacheSequence=nextSequence;
 Guard l;cacheNote=ok?"":"SD metadata cache write failed";return ok;
}
void cacheService(){
 fs::FS* fs=sdFs();
 {Guard l;cacheStorageReady=fs!=nullptr;
  if(!fs){cacheNote="No SD card; RSS list is RAM only";cacheFs=nullptr;return;}
 }
 bool load;uint32_t g;String url;
 {Guard l;
  if(!enabled)return;
  load=cachePending||fs!=cacheFs;g=generation;url=feedUrl;
  if(!load&&!cacheDirty)return;
  if(!load&&(int32_t)(millis()-cacheRetryAt)<0)return;
  cachePending=false;
 }
 if(load){
  cacheFs=fs;cacheSequence=0;
  bool loaded=cacheLoad(*fs,g,url);
  Guard l;cacheNote="";
  cacheDirty=!loaded&&!episodes.empty();
 }
 if(cacheDirty){
  cacheDirty=!cacheSave(*fs,g,url);
  cacheRetryAt=millis()+5000;
 }
}

// Body reader supports Content-Length, close-delimited and chunked responses.
// It lives exclusively on the worker, including DNS/TLS/redirect waits.
class RadioSecureClient : public WiFiClientSecure {
public:
 String lastHost, diagnostic;
 int connect(const char* host,uint16_t port,int32_t timeout) override {
  lastHost=host;diagnostic="";IPAddress ip;uint32_t began=millis();
  if(!WiFi.hostByName(host,ip)){
   diagnostic="DNS lookup failed for "+String(host);return 0;
  }
  if(ip.type()==IPv4 && ip[0]==198 && (ip[1]==18||ip[1]==19)){
   diagnostic="DNS returned proxy Fake-IP "+ip.toString()+" for "+String(host)+"; check Wi-Fi DNS";
   Serial.println("radio: "+diagnostic);return 0;
  }
  Serial.printf("radio: DNS %s -> %s (%lu ms)\n",host,ip.toString().c_str(),(unsigned long)(millis()-began));
  // stop_ssl_socket() clears bundle_attach_cb, including on HTTP redirects.
  // Rebind before EVERY handshake; retaining _use_ca_bundle alone is insufficient.
  setCACertBundle(radioCaStart,(size_t)(radioCaEnd-radioCaStart));
  int ok=WiFiClientSecure::connect(host,port,timeout);
  if(!ok){
   char message[192]={};int code=lastError(message,sizeof message);
   diagnostic=String(host)+" ip="+ip.toString()+" tls="+String(code)+" "+String(message);
   Serial.println("radio: connect failed "+diagnostic);
  }
  return ok;
 }
};
class Reader {
 WiFiClient plain;RadioSecureClient tls;HTTPClient http;
 uint32_t gen,lastByte=0,chunk=0;int remaining=-1;
 bool chunked=false,needCRLF=false,ended=false;
 int raw(uint8_t* out,size_t n){
  auto* stream=http.getStreamPtr();
  while(alive(gen)&&WiFi.status()==WL_CONNECTED){
   int av=stream->available();
   if(av>0){int got=stream->read(out,std::min(n,(size_t)av));if(got>0){lastByte=millis();return got;}}
   if(!stream->connected()){ended=true;return 0;}
   if((int32_t)(millis()-lastByte)>15000){problem="network timeout";return -1;}
   vTaskDelay(pdMS_TO_TICKS(5));
  }problem="cancelled or Wi-Fi disconnected";return -1;
 }
 bool line(String& out){out="";uint8_t c;while(out.length()<128){if(raw(&c,1)!=1)return false;if(c=='\n')return true;if(c!='\r')out+=(char)c;}problem="invalid chunk header";return false;}
public:
 String problem;int status=0;size_t total=0;
 explicit Reader(uint32_t g):gen(g){}
 ~Reader(){http.end();}
 bool open(const String& url,uint64_t offset=0){
  http.end();problem="";status=0;total=0;chunk=0;remaining=-1;
  chunked=needCRLF=ended=false;
  if(!urlOK(url)){problem="invalid HTTP(S) URL";return false;}
  http.setConnectTimeout(15000);http.setTimeout(5000);http.setReuse(false);http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);http.setRedirectLimit(5);
  http.setUserAgent("Mozilla/5.0 AgentTouch/1.0");
  bool ok;
  if(url.startsWith("https://")){
   tls.setCACertBundle(radioCaStart,(size_t)(radioCaEnd-radioCaStart));tls.setHandshakeTimeout(15);
   ok=http.begin(tls,url);
  }else ok=http.begin(plain,url);
  if(!ok){problem="could not open URL";return false;}
  const char* names[]={"Transfer-Encoding","Content-Range","Content-Type"};http.collectHeaders(names,3);
  if(offset)http.addHeader("Range","bytes="+String((unsigned long)offset)+"-");
  status=http.GET();
  if(status!=200&&status!=206){
   problem="HTTP "+String(status);
   if(status<0){problem+=" "+HTTPClient::errorToString(status);if(tls.diagnostic.length())problem+=" / "+tls.diagnostic;}
   else if(tls.lastHost.length())problem+=" / "+tls.lastHost;
   return false;
  }
  if(offset&&status!=206){problem="audio server does not support seeking";return false;}
  remaining=http.getSize();String te=http.header("Transfer-Encoding");te.toLowerCase();chunked=te.indexOf("chunked")>=0;
  lastByte=millis();return true;
 }
 int read(uint8_t* out,size_t n){
  if(ended||remaining==0)return 0;
  if(chunked){
   if(!chunk){
    String header;if(needCRLF&&!line(header))return -1;
    if(!line(header))return -1;char* end=nullptr;unsigned long value=strtoul(header.c_str(),&end,16);
    if(end==header.c_str()||(*end&&*end!=';'&&!isspace((unsigned char)*end))){problem="invalid chunk size";return -1;}
    chunk=value;if(!chunk){ended=true;return 0;}needCRLF=true;
   }
   int got=raw(out,std::min(n,(size_t)chunk));if(got>0){chunk-=got;total+=got;}return got;
  }
  if(remaining>0)n=std::min(n,(size_t)remaining);
  int got=raw(out,n);if(got>0){if(remaining>0)remaining-=got;total+=got;}
  else if(!got&&remaining>0){problem="audio transfer ended early";return -1;}
  return got;
 }
};

bool clockReady(uint32_t g){
 if(time(nullptr)>1700000000)return true;
 configTime(0,0,"time.apple.com","pool.ntp.org");uint32_t start=millis();
 while(alive(g)&&(int32_t)(millis()-start)<10000){if(time(nullptr)>1700000000)return true;vTaskDelay(pdMS_TO_TICKS(50));}
 return false;
}
void fetchFeed(uint32_t g,const String& url){
 if(!clockReady(g)){fail(g,"Clock sync failed; check Wi-Fi internet");return;}
 Reader reader(g);if(!reader.open(url)){fail(g,reader.problem);return;}
 radiofeed::Parser parser;uint8_t block[2048];
 while(alive(g)&&parser.items.size()<radiofeed::CACHE_LIMIT){
  int n=reader.read(block,sizeof block);if(n<0){fail(g,reader.problem);return;}if(!n)break;
  parser.write((const char*)block,(size_t)n);if(parser.invalid){fail(g,"RSS is too large or malformed");return;}
 }
 if(!alive(g))return;
 if(parser.items.empty()){fail(g,"RSS has no audio enclosure items");return;}
 bool changed=true;
 {Guard l;if(g!=generation)return;
  auto merged=radiofeed::mergeCache(parser.items,episodes);
  changed=!radiofeed::sameCache(merged,episodes)||show!=String(parser.show.c_str())||cacheNote.length();
  episodes=std::move(merged);show=parser.show.c_str();selected=0;
  refresh=false;state="idle";error="";bitrate=0;
 }
 if(changed)cacheDirty=true;
 cacheService();
}

bool putSamples(uint32_t g,const int16_t* samples,uint32_t n){
 uint32_t off=0;
 while(off<n){
  cacheService(); // also handles a card inserted while streaming/paused
  uint32_t written=0;
  {Guard l;if(g!=generation||!enabled)return false;
   written=std::min(n-off,CAP-fill);
   uint32_t first=std::min(written,CAP-head);
   if(first)memcpy(ring+head,samples+off,first*sizeof(int16_t));
   if(written>first)memcpy(ring,samples+off+first,(written-first)*sizeof(int16_t));
   head=(head+written)%CAP;fill+=written;off+=written;
  }
  if(!written)vTaskDelay(pdMS_TO_TICKS(10));
 }
 return true;
}
void playStream(uint32_t g,const radiofeed::Episode& ep,uint32_t position,int rateKbps){
 if(!clockReady(g)){fail(g,"Clock sync failed");return;}
 uint32_t tagOffset;{Guard l;tagOffset=audioStart;}
 uint64_t offset=position&&rateKbps>0?tagOffset+(uint64_t)position*rateKbps*125:0;
 bool probeSeek=position&&!offset; // learn MP3 bitrate before the first seek
 Reader reader(g);if(!reader.open(ep.url.c_str(),offset)){fail(g,reader.problem);return;}
 std::unique_ptr<drmp3dec,decltype(&free)> decoder((drmp3dec*)ps_malloc(sizeof(drmp3dec)),free);
 std::unique_ptr<uint8_t,decltype(&free)> input((uint8_t*)ps_malloc(32768),free);
 std::unique_ptr<int16_t,decltype(&free)> pcm((int16_t*)ps_malloc(DRMP3_MAX_SAMPLES_PER_FRAME*sizeof(int16_t)),free);
 if(!decoder||!input||!pcm){fail(g,"Not enough RAM for MP3 decoder");return;}
 drmp3dec_init(decoder.get());RadioResampler resampler;
 int used=0;
 // ID3 covers can be much larger than the compressed-frame buffer. Skip the
 // declared tag in bounded blocks rather than interpreting cover bytes as MP3.
 if(!offset){
  while(used<10){int n=reader.read(input.get()+used,10-used);if(n<=0){fail(g,"Truncated MP3 header");return;}used+=n;}
  if(!memcmp(input.get(),"ID3",3)){
   uint32_t size=0;for(int i=6;i<10;++i){if(input.get()[i]&0x80){fail(g,"Invalid ID3 size");return;}size=(size<<7)|input.get()[i];}
   if(input.get()[3]==4&&(input.get()[5]&0x10))size+=10;
   if(size>8*1024*1024){fail(g,"ID3 metadata too large");return;}
   uint32_t left=size;while(left&&alive(g)){int n=reader.read(input.get(),std::min<uint32_t>(left,32768));if(n<=0){fail(g,reader.problem.length()?reader.problem:"Truncated ID3 tag");return;}left-=n;}
   {Guard l;if(g!=generation)return;audioStart=size+10;}
   used=0;
  }else{Guard l;if(g==generation)audioStart=0;}
 }
 bool done=false,decoded=false;int16_t mono[1024];uint32_t count=0;
 uint32_t noAudioAt=millis();
 auto emit=[&](int16_t value){mono[count++]=value;if(count==1024){if(!putSamples(g,mono,count))return false;count=0;}return true;};
 while(alive(g)){
  if(!done&&used<16384){int n=reader.read(input.get()+used,32768-used);if(n<0){fail(g,reader.problem);return;}if(n)used+=n;else done=true;}
  if(!used&&done)break;
  drmp3dec_frame_info frame={};int samples=drmp3dec_decode_frame(decoder.get(),input.get(),used,pcm.get(),&frame);
  int consumed=frame.frame_bytes;
  if(consumed>0&&consumed<=used){used-=consumed;memmove(input.get(),input.get()+consumed,used);}
  else if(done)break;
  else if(used>=16384){memmove(input.get(),input.get()+used-3,3);used=3;}
  if(samples>0){
   if(probeSeek){
    // Reuse the connection/decoder buffers: a second live TLS connection
    // would consume scarce internal RAM. Range seeking remains approximate.
    if(frame.bitrate_kbps<=0){fail(g,"Could not determine MP3 bitrate");return;}
    {Guard l;if(g!=generation)return;bitrate=frame.bitrate_kbps;tagOffset=audioStart;}
    offset=tagOffset+(uint64_t)position*frame.bitrate_kbps*125;
    if(!reader.open(ep.url.c_str(),offset)){fail(g,reader.problem);return;}
    drmp3dec_init(decoder.get());used=0;done=false;probeSeek=false;
    noAudioAt=millis();continue;
   }
   decoded=true;noAudioAt=millis();
   {Guard l;if(g!=generation)return;bitrate=frame.bitrate_kbps;}
   if(!resampler.push(pcm.get(),samples,frame.channels,frame.sample_rate,emit))return;
  }else if((int32_t)(millis()-noAudioAt)>15000){fail(g,"No MP3 audio found (MP3 URLs only)");return;}
  taskYIELD();
 }
 if(!alive(g))return;
 if(!decoded){fail(g,"No supported MP3 audio found");return;}
 if(count&&!putSamples(g,mono,count))return;
 Guard l;if(g==generation)eof=true;
}

void worker(void*){
 uint32_t handled=0;
 for(;;){
  cacheService();
  uint32_t g,pos;bool doRefresh,play;String url;radiofeed::Episode ep;int kbps;
  {Guard l;g=generation;url=feedUrl;doRefresh=enabled&&refresh;pos=baseSec;kbps=bitrate;
   play=enabled&&want&&!refresh&&selected>=0&&selected<(int)episodes.size();
   if(play)ep=episodes[selected];
  }
  if(g==handled||(!doRefresh&&!play)){vTaskDelay(pdMS_TO_TICKS(30));continue;}
  if(WiFi.status()!=WL_CONNECTED){
   {Guard l;if(g==generation)state="offline";}
   vTaskDelay(pdMS_TO_TICKS(100));continue;
  }
  handled=g;
  if(doRefresh)fetchFeed(g,url);
  else {
   {Guard l;decoding=true;}
   playStream(g,ep,pos,kbps);
   {Guard l;decoding=false;}
  }
 }
}
}

bool radioInit(){
 if(initialized)return true;
 mux=xSemaphoreCreateMutex();if(!mux)return false;
 ring=(int16_t*)ps_malloc(CAP*sizeof(int16_t));if(!ring)return false;
 prefs.begin("radio",false);feedUrl=prefs.getString("feed","");enabled=prefs.getBool("enabled",false)&&urlOK(feedUrl);
 refresh=enabled;state=enabled?"loading":"idle";
 initialized=xTaskCreatePinnedToCore(worker,"rss-mp3",16384,nullptr,1,nullptr,0)==pdPASS;
 return initialized;
}
bool radioEnabled(){if(!initialized)return false;Guard l;return enabled;}
bool radioConfigure(const char* url,bool on){
 if(!initialized)return false;String value=url?url:"";value.trim();if((on||value.length())&&!urlOK(value))return false;
 {Guard l;bool changed=value!=feedUrl;enabled=on;feedUrl=value;++generation;want=false;
  if(changed){episodes.clear();selected=-1;show="";}
  cachePending=on&&episodes.empty();refresh=on&&episodes.empty();bitrate=0;baseSec=0;clearBuffer();error="";state=refresh?"loading":"idle";
 }
 prefs.putString("feed",value);prefs.putBool("enabled",on);return true;
}
void radioDisable(){if(!initialized)return;String saved;{Guard l;saved=feedUrl;}radioConfigure(saved.c_str(),false);}
void radioRefresh(){if(!initialized)return;Guard l;if(!enabled)return;++generation;cachePending=false;want=false;refresh=true;baseSec=0;clearBuffer();state="loading";error="";}
void radioSelect(int i,bool play){if(!initialized)return;Guard l;if(i<0||i>=(int)episodes.size())return;
 selected=i;++generation;want=play;refresh=false;baseSec=0;bitrate=0;audioStart=0;clearBuffer();state=play?"buffering":"paused";error="";}
void radioSkip(int delta){if(!initialized)return;int i,n;bool playing;{Guard l;i=selected;n=episodes.size();playing=want;}if(n)radioSelect((i+delta+n)%n,playing);}
void radioToggle(){if(!initialized)return;Guard l;if(!enabled||episodes.empty()||refresh)return;
 if(want){want=false;state="paused";}else{want=true;if(state=="error"||(!decoding&&!eof)){++generation;clearBuffer();}state=buffering?"buffering":"playing";}}
void radioPause(){if(!initialized)return;Guard l;want=false;if(enabled)state="paused";}
static bool radioSeekLocked(uint32_t pos){
 if(!enabled||selected<0||selected>=(int)episodes.size()||refresh)return false;
 if(episodes[selected].duration&&pos>=episodes[selected].duration)pos=episodes[selected].duration-1;
 baseSec=pos;++generation;clearBuffer();state=want?"buffering":"paused";error="";return true;
}
void radioSeek(int32_t delta){if(!initialized)return;Guard l;
 int64_t pos=(int64_t)baseSec+played/RATE+delta;if(pos<0)pos=0;
 radioSeekLocked((uint32_t)pos);}
bool radioSeekTo(uint32_t seconds){if(!initialized)return false;Guard l;return radioSeekLocked(seconds);}
bool radioPlaying(){if(!initialized)return false;Guard l;return enabled&&want;}
bool radioWantPlay(){if(!initialized)return false;Guard l;return enabled&&want&&!buffering&&fill>0;}
uint32_t radioPull(int16_t* out,uint32_t maxSamples){
 if(!initialized)return 0;Guard l;if(!enabled||!want||buffering)return 0;
 if(!fill){if(!eof){buffering=true;state="buffering";++underruns;}return 0;}
 uint32_t n=std::min(fill,maxSamples),first=std::min(n,CAP-tail);
 memcpy(out,ring+tail,first*sizeof(int16_t));if(n>first)memcpy(out+first,ring,(n-first)*sizeof(int16_t));
 tail=(tail+n)%CAP;fill-=n;played+=n;return n;
}
int radioCount(){if(!initialized)return 0;Guard l;return episodes.size();}
bool radioEpisode(int i,char* title,size_t size,unsigned* duration){
 if(!initialized)return false;Guard l;if(i<0||i>=(int)episodes.size())return false;
 copyText(title,size,episodes[i].title.c_str());if(duration)*duration=episodes[i].duration;return true;
}
const PlayerInfo& radioInfo(){
 memset(&info,0,sizeof info);info.index=-1;if(!initialized)return info;
 Guard l;info.hasItems=!episodes.empty();info.playing=want;info.index=selected;info.posSec=baseSec+played/RATE;
 if(selected>=0&&selected<(int)episodes.size()){copyText(info.title,sizeof info.title,episodes[selected].title.c_str());info.durSec=episodes[selected].duration;}
 String status=show;if(state!="playing"&&state!="idle")status+=(status.length()?" / ":"")+state;
 if(error.length())status=error;copyText(info.show,sizeof info.show,status.c_str());return info;
}
void radioPoll(){
 if(!initialized)return;bool kick=false,next=false;int index=-1;
 {Guard l;if(!enabled)return;
  if(want&&buffering&&(fill>=PREBUFFER||(eof&&fill))){buffering=false;state="playing";}
  kick=want&&!buffering&&fill>0;
  if(want&&eof&&!fill&&!episodes.empty()){next=true;index=(selected+1)%episodes.size();}
 }
 if(next)radioSelect(index,true);else if(kick&&!playerRunning())audioPlayerKick();
}
void radioStatus(JsonObject out){
 out["tls_ca_reattach"]=true;out["tls_psram"]=true;out["internal_free"]=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);out["internal_largest"]=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);out["supported"]=true;out["cache_limit"]=radiofeed::CACHE_LIMIT;out["ready"]=initialized.load();if(!initialized)return;
 Guard l;out["cache_storage"]=cacheStorageReady;out["cache_error"]=cacheNote;out["enabled"]=enabled;out["feed"]=feedUrl;out["state"]=state;out["error"]=error;
 out["wifi"]=WiFi.status()==WL_CONNECTED;out["count"]=episodes.size();out["index"]=selected;
 out["pos"]=baseSec+played/RATE;out["buffer_s"]=(double)fill/RATE;out["underruns"]=underruns;

}
