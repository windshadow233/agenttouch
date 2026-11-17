#include "radio.h"
#include "audio.h"
#include "rss_feed.h"
#include "rss_cache.h"
#include "rss_fetch.h"
#include "http_range.h"
#include "i18n.h"
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
constexpr uint32_t RATE=16000,CAP=RATE*30,PREBUFFER=RATE*8;
uint32_t prebufferTarget=PREBUFFER,streamUnderruns=0;
SemaphoreHandle_t mux=nullptr;
struct Guard {Guard(){xSemaphoreTake(mux,portMAX_DELAY);}~Guard(){xSemaphoreGive(mux);}};
Preferences prefs;
std::atomic<bool> initialized{false};
std::atomic<uint32_t> rssBytes{0},rssRequests{0},rssItems{0};
std::atomic<int> rssHttpStatus{0};
std::atomic<bool> rssParsing{false};
std::atomic<uint32_t> rssDownloadMs{0},rssParseMs{0},rssReadFree{0},rssReadLargest{0};
std::atomic<uint32_t> audioNetBytes{0},audioNetWaitMax{0},audioReconnects{0};
bool enabled=false,want=false,refresh=false,eof=false,buffering=true,decoding=false;
uint32_t feedGeneration=1; // RSS requests are independent of playback cancellation
uint32_t generation=1,head=0,tail=0,fill=0,played=0,baseSec=0,underruns=0,audioStart=0;
int selected=-1,bitrate=0;
int16_t* ring=nullptr;
String feedError;
uint32_t fetchDoneUntil=0;
unsigned fetchDoneCount=0,fetchDoneNew=0;
String feedUrl,error,state="idle",show;
std::vector<radiofeed::Episode> episodes;
PlayerInfo info={};
String lastEpisode;
uint32_t resumeSavedAt=0;
int restoredSelection(){
 int index=radiofeed::episodeIndex(episodes,std::string(lastEpisode.c_str()));
 return index>=0?index:0;
}
void saveSelection(){
 if(selected<0||selected>=(int)episodes.size())return;
 String url=episodes[selected].url.c_str();
 if(url!=lastEpisode){lastEpisode=url;prefs.putString("last_episode",url);}
}

// Bounded NVS history, independent of the SD metadata cache and list order.
struct ResumePoint {uint64_t id;uint32_t seconds,used;};
ResumePoint resumePoints[100]={};
uint32_t resumeClock=0;
uint64_t episodeId(const std::string& url){
 uint64_t hash=14695981039346656037ULL;
 for(unsigned char ch:url){hash^=ch;hash*=1099511628211ULL;}
 return hash?hash:1;
}
uint32_t resumePosition(){
 if(selected<0||selected>=(int)episodes.size())return 0;
 const auto& ep=episodes[selected];uint64_t id=episodeId(ep.url);
 for(const auto& point:resumePoints)if(point.id==id)
  return ep.duration&&point.seconds>=ep.duration?0:point.seconds;
 return 0;
}
// Caller holds mux. Save the consumed position, never the decoder's read-ahead.
void saveResume(){
 if(selected<0||selected>=(int)episodes.size())return;
 saveSelection();
 uint64_t id=episodeId(episodes[selected].url);
 uint32_t seconds=eof&&!fill?0:baseSec+played/RATE;
 ResumePoint* slot=&resumePoints[0];
 for(auto& point:resumePoints){
  if(point.id==id){slot=&point;break;}
  if(!point.id||point.used<slot->used)slot=&point;
 }
 if(slot->id==id&&slot->seconds==seconds)return;
 if(!seconds&&slot->id!=id)return;
 *slot={id,seconds,++resumeClock};
 prefs.putBytes("resume_v1",resumePoints,sizeof resumePoints);
}

bool urlOK(const String& u){return radiofeed::urlOK(std::string(u.c_str()))&&u.indexOf('\n')<0&&u.indexOf('\r')<0&&u.indexOf(' ')<0;}
void copyText(char* out,size_t size,const char* text){auto s=radiofeed::clipped(text?std::string(text):std::string(),size-1);memcpy(out,s.data(),s.size());out[s.size()]=0;}
bool alive(uint32_t g){Guard l;return enabled&&generation==g;}
bool feedAlive(uint32_t g){Guard l;return enabled&&feedGeneration==g&&refresh;}
void feedFail(uint32_t g,const String& why){
 Guard l;if(g!=feedGeneration)return;
 feedError=why;refresh=false;fetchDoneUntil=0;
 if(!want && episodes.empty())state="error";
}
void fail(uint32_t g,const String& why){Guard l;if(g==generation){error=why;state="error";want=false;}}
void clearBuffer(){head=tail=fill=played=0;buffering=true;eof=false;prebufferTarget=PREBUFFER;streamUnderruns=0;}

// Metadata only, on an already-mounted SD card. Never format storage here.
bool cacheStorageReady=false,cachePending=true;
bool importPending=false;
bool cacheDirty=false; // owned exclusively by the RSS/cache worker
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
bool cacheLoad(fs::FS& fs,uint32_t g,const String& url,bool imported=false){
 std::vector<radiofeed::Episode> best;String cachedShow;uint64_t bestSeq=0;
 const char* paths[2]={imported?"/agentpet/rss-import.json":CACHE_SLOTS[0],imported?"/agentpet/rss-import.json":CACHE_SLOTS[1]};
 for(const char* path:paths){
  File file=fs.open(path,FILE_READ);if(!file){if(file)file.close();continue;}
  CacheAllocator allocator;JsonDocument doc(&allocator);auto err=deserializeJson(doc,file);file.close();if(err)continue;
  uint64_t seq=doc["seq"] | (uint64_t)0;
  cacheSequence=std::max(cacheSequence,seq); // worker owns the disk sequence
  if((doc["version"] | 0)!=1||String(doc["feed"] | "")!=url||seq<bestSeq)continue;
  auto items=doc["items"].as<JsonArray>();if(items.isNull())continue;
  std::vector<radiofeed::Episode> valid;
  for(JsonObject item:items){radiofeed::Episode e;e.url=item["url"] | "";e.title=item["title"] | "";e.duration=item["dur"] | 0u;
   if(!radiofeed::urlOK(e.url)||e.title.size()>191||e.duration>7*86400u){valid.clear();break;}
   valid.push_back(e);
  }
  if(!valid.empty()){best=radiofeed::mergeCache(std::move(valid),{});bestSeq=seq;cachedShow=doc["show"] | "";}
 }
 if(best.empty())return false;
 Guard l;if(g!=generation||url!=feedUrl)return false;
 // Inserting a card during playback must not replace the current selection.
 if(!imported&&!episodes.empty())return radiofeed::sameCache(episodes,best)&&show==cachedShow;
 episodes=std::move(best);show=cachedShow;selected=restoredSelection();baseSec=resumePosition();refresh=false;want=false;state="idle";error="";
 return true;
}
bool cacheSave(fs::FS& fs,uint32_t g,const String& url){
 uint64_t nextSequence=cacheSequence+1;
 CacheAllocator allocator;JsonDocument doc(&allocator);doc["version"]=1;doc["feed"]=url;doc["seq"]=nextSequence;
 {Guard l;if(g!=generation||url!=feedUrl||episodes.empty())return false;doc["show"]=show;
  JsonArray items=doc["items"].to<JsonArray>();for(const auto& e:episodes){JsonObject item=items.add<JsonObject>();item["title"]=e.title.c_str();item["url"]=e.url.c_str();item["dur"]=e.duration;}
 }
 if(doc.overflowed()){Guard l;cacheNote="metadata cache too large";return false;}
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
 uint32_t gen,lastByte=0,chunk=0;
 int remaining=-1;
 bool feedReader=false,chunked=false,needCRLF=false,ended=false,recoverable=false,resumeRejected=false;
 String requestUrl,validator;
 uint64_t startOffset=0,resourceSize=0,segmentEnd=0;
 bool active(){return feedReader?feedAlive(gen):alive(gen);}
 uint64_t position(){return startOffset+total;}
 String decimal(uint64_t value){char s[24];snprintf(s,sizeof(s),"%llu",(unsigned long long)value);return String(s);}
 int raw(uint8_t* out,size_t n){
  auto* stream=http.getStreamPtr();
  if(!stream){problem="HTTP stream disconnected";recoverable=true;return -1;}
  while(active()&&WiFi.status()==WL_CONNECTED){
   int av=stream->available();
   if(av>0){int got=stream->read(out,std::min(n,(size_t)av));if(got>0){lastByte=millis();return got;}}
   if(!stream->connected()){ended=true;recoverable=true;return 0;}
   if((int32_t)(millis()-lastByte)>15000){problem="network timeout";recoverable=true;return -1;}
   vTaskDelay(pdMS_TO_TICKS(5));
  }
  recoverable=active();problem="cancelled or Wi-Fi disconnected";return -1;
 }
 bool line(String& out){
  out="";uint8_t c;
  while(out.length()<128){
   int got=raw(&c,1);if(got!=1){if(!got)problem="HTTP chunk header ended early";return false;}
   if(c=='\n')return true;if(c!='\r')out+=(char)c;
  }
  problem="invalid chunk header";return false;
 }
 bool openAt(uint64_t offset){
  // A Wi-Fi outage must not discard the reader's offset or RSS parser.
  uint32_t waiting=millis();
  while(active()&&WiFi.status()!=WL_CONNECTED&&(int32_t)(millis()-waiting)<30000)
   vTaskDelay(pdMS_TO_TICKS(100));
  if(!active()||WiFi.status()!=WL_CONNECTED){status=0;problem="Wi-Fi disconnected; resume pending";return false;}
  http.end();problem="";status=0;chunk=0;remaining=-1;
  chunked=needCRLF=ended=recoverable=false;
  http.setConnectTimeout(15000);http.setTimeout(5000);http.setReuse(false);http.useHTTP10(false);
  http.setAcceptEncoding("identity");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);http.setRedirectLimit(5);
  http.setUserAgent("Mozilla/5.0 AgentTouch/1.0");
  bool ok;
  if(requestUrl.startsWith("https://")){
   tls.setCACertBundle(radioCaStart,(size_t)(radioCaEnd-radioCaStart));tls.setHandshakeTimeout(15);
   ok=http.begin(tls,requestUrl);
  }else ok=http.begin(plain,requestUrl);
  if(!ok){problem="could not open URL";return false;}
  const char* names[]={"Transfer-Encoding","Content-Range","Content-Type","Content-Encoding","ETag","Last-Modified"};
  http.collectHeaders(names,6);
  if(offset){
   String range="bytes="+decimal(offset)+"-";
   http.addHeader("Range",range);
   if(offset&&validator.length())http.addHeader("If-Range",validator);
  }
  if(feedReader)rssRequests++;
  status=http.GET();if(feedReader)rssHttpStatus=status;
  if(status!=200&&status!=206){
   problem="HTTP "+String(status);
   if(status<0){
    problem+=" "+HTTPClient::errorToString(status);
    char message[128]={};int code=tls.lastError(message,sizeof(message));
    if(code)problem+=" / TLS "+String(code)+" "+String(message);
    if(tls.diagnostic.length())problem+=" / "+tls.diagnostic;
   }
   return false;
  }
  String encoding=http.header("Content-Encoding");encoding.toLowerCase();
  if(encoding.length()&&encoding!="identity"){resumeRejected=true;problem="unsupported HTTP content encoding: "+encoding;return false;}
  if(offset&&status!=206){resumeRejected=true;problem="server ignored HTTP resume range; tap Fetch to restart";return false;}
  int length=http.getSize();
  if(status==206){
   radiohttp::Range range;
   if(!radiohttp::parseRange(http.header("Content-Range").c_str(),range)||
      !radiohttp::validRange(range,offset,resourceSize,length,0)){
    resumeRejected=true;problem="invalid HTTP resume range";return false;
   }
   resourceSize=range.size;segmentEnd=range.last+1;
   if(range.last-range.first+1>INT32_MAX){problem="HTTP range too large";return false;}
   remaining=(int)(range.last-range.first+1);
  }else{
   resourceSize=length>0?(uint64_t)length:0;segmentEnd=resourceSize;remaining=length;
  }
  String nextValidator=http.header("ETag");
  if(nextValidator.startsWith("W/"))nextValidator="";
  if(!nextValidator.length())nextValidator=http.header("Last-Modified");
  if(offset&&validator.length()&&nextValidator.length()&&validator!=nextValidator){
   resumeRejected=true;problem="HTTP resource changed during transfer; tap Fetch to restart";return false;
  }
  if(!validator.length())validator=nextValidator;
  String te=http.header("Transfer-Encoding");te.toLowerCase();chunked=te.indexOf("chunked")>=0;
  if(chunked)remaining=-1;
  lastByte=millis();return true;
 }
 bool reopenAt(uint64_t offset){
  for(int attempt=0;attempt<3 && active();attempt++){
   if(openAt(offset))return true;
   // Do not append after a wrong range/version, or retry permanent HTTP errors.
   if(status>=200 && status<500 && status!=408 && status!=429)return false;
   if(attempt<2)vTaskDelay(pdMS_TO_TICKS(300));
  }
  return false;
 }
 int readOnce(uint8_t* out,size_t n){
  if(ended||remaining==0)return 0;
  if(chunked){
   if(!chunk){
    String header;if(needCRLF&&!line(header))return -1;
    if(!line(header))return -1;char* end=nullptr;unsigned long value=strtoul(header.c_str(),&end,16);
    if(end==header.c_str()||(*end&&*end!=';'&&!isspace((unsigned char)*end))){problem="invalid chunk size";return -1;}
    chunk=value;if(!chunk){ended=true;return 0;}needCRLF=true;
   }
   int got=raw(out,std::min(n,(size_t)chunk));
   if(got>0)chunk-=got;
   else if(!got){problem="HTTP chunk ended early";return -1;}
   return got;
  }
  if(remaining>0)n=std::min(n,(size_t)remaining);
  int got=raw(out,n);
  if(got>0){if(remaining>0)remaining-=got;}
  else if(!got&&remaining>0){
   problem="HTTP body ended early: received "+decimal(total)+", missing "+String(remaining)+" bytes";return -1;
  }
  return got;
 }
public:
 String problem;int status=0;size_t total=0;
 explicit Reader(uint32_t g,bool feed=false):gen(g),feedReader(feed){}
 ~Reader(){http.end();}
 bool open(const String& url,uint64_t offset=0){
  if(!urlOK(url)){problem="invalid HTTP(S) URL";return false;}
  requestUrl=url;startOffset=offset;total=0;resourceSize=0;validator="";resumeRejected=false;
  if(!feedReader){audioNetBytes=0;audioNetWaitMax=0;audioReconnects=0;}
  // Stream one response through the small parser buffer; Range is only for resume.
  return openAt(offset);
 }
 bool retryable()const{
  return !resumeRejected && !(status>=400&&status<500&&status!=408&&status!=429);
 }
 bool resume(){
  if(!retryable())return false;
  if(!validator.length()){
   resumeRejected=true;problem="RSS server has no resume validator; tap Fetch to restart";return false;
  }
  return reopenAt(position());
 }
 bool buffered(){
  if(ended || remaining==0)return false;
  auto* stream=http.getStreamPtr();return stream && stream->available()>0;
 }
 int read(uint8_t* out,size_t n){
  if(!n)return 0;
  uint32_t began=millis();
  unsigned retries=0;
  while(active()){
   // A fully received range is a segment boundary, not the end of the RSS.
   if((remaining==0 || (ended&&!recoverable&&position()==segmentEnd)) && resourceSize && position()<resourceSize){
    if(!reopenAt(position()))return -1;
   }
   int got=readOnce(out,n);
   if(got>0){
    total+=got;
    if(feedReader)rssBytes=total;
    else {audioNetBytes+=got;audioNetWaitMax=std::max(audioNetWaitMax.load(),(uint32_t)(millis()-began));}
    return got;
   }
   if(!got && ((!resourceSize&&!recoverable)||(resourceSize&&position()>=resourceSize)))return 0;
   if(!recoverable || !validator.length() || retries++>=3)return -1;
   // Reconnect at the exact byte already delivered to the parser/decoder.
   // The existing PCM ring continues to play while the connection is restored.
   String cause=problem;if(!feedReader)audioReconnects++;
   Serial.printf("radio: %s resume at %llu: %s\n",feedReader?"RSS":"audio",(unsigned long long)position(),cause.c_str());
   bool opened=false;
   while(active() && retries<=3){
    vTaskDelay(pdMS_TO_TICKS(150));
    if(openAt(position())){opened=true;break;}
    // A mismatching range or changed entity must never be appended.
    if(status>=200 || !active())break;
    retries++;
   }
   if(!opened)return -1;
  }
  problem="cancelled";return -1;
 }
};

bool clockReady(uint32_t g,bool feed=false){
 if(time(nullptr)>1700000000)return true;
 configTime(0,0,"time.apple.com","pool.ntp.org");uint32_t start=millis();
 while((feed?feedAlive(g):alive(g))&&(int32_t)(millis()-start)<10000){if(time(nullptr)>1700000000)return true;vTaskDelay(pdMS_TO_TICKS(50));}
 return false;
}
void fetchFeed(uint32_t g,const String& url){
 rssBytes=0;rssItems=0;rssRequests=0;rssHttpStatus=0;rssParsing=false;rssDownloadMs=0;rssParseMs=0;rssReadFree=0;rssReadLargest=0;
 if(!clockReady(g,true)){feedFail(g,"Clock sync failed; check Wi-Fi internet");return;}
 struct FeedReader : Reader {
  String url;
  FeedReader(uint32_t g,const String& u):Reader(g,true),url(u){}
  bool open(){return Reader::open(url);}
 };
 radiofeed::Parser parser;std::string problem;
 auto makeReader=[&](){return std::unique_ptr<FeedReader>(new FeedReader(g,url));};
 auto active=[&](){return feedAlive(g);};
 auto pause=[&](){for(int i=0;i<10 && feedAlive(g);i++)vTaskDelay(pdMS_TO_TICKS(50));};
 bool ok=false;
 fs::FS* fs=sdFs();
 if(fs){
  constexpr const char* temporary="/agentpet/rss-download.tmp";
  constexpr const char* saved="/agentpet/rss.xml";
  if(!fs->exists("/agentpet")&&!fs->mkdir("/agentpet")){feedFail(g,"SD RSS directory unavailable");return;}
  constexpr const char* batchPath="/agentpet/rss-items.tmp";
  File batches=fs->open(batchPath,FILE_WRITE);
  if(!batches){feedFail(g,"SD RSS metadata file unavailable");return;}
  // Each deliberate Fetch starts a new file; automatic retries append in place.
  File file=fs->open(temporary,FILE_WRITE);
  if(!file){feedFail(g,"SD RSS file unavailable");return;}
  // Batch small TLS reads into 16 KB SD writes, allocated outside internal RAM.
  std::unique_ptr<uint8_t,decltype(&free)> transfer((uint8_t*)ps_malloc(16384),free);
  if(!transfer){file.close();feedFail(g,"Not enough PSRAM for RSS transfer");return;}
  // Reserve DMA-capable memory before the episode list starts growing.
  std::unique_ptr<uint8_t,decltype(&free)> sdRead((uint8_t*)heap_caps_malloc(2048,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA),free);
  if(!sdRead){file.close();feedFail(g,"Not enough internal RAM for SD read");return;}
  uint32_t started=millis(),parseStarted=0;
  auto prepareRead=[&](File& disk,size_t offset){
   disk.close();disk=fs->open(temporary,FILE_READ);
   if(!disk)return false;
   disk.setBufferSize(512);
   return disk.size()==rssBytes.load()&&disk.seek(offset);
  };
  auto readSaved=[&](File& disk,uint8_t* out,size_t count){
   int n=(int)disk.read(sdRead.get(),std::min(count,(size_t)2048));
   int savedError=errno;
   rssReadFree=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
   rssReadLargest=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
   if(n>0)memcpy(out,sdRead.get(),n);
   errno=savedError;return n;
  };
  auto saveBatch=[&](std::vector<radiofeed::Episode>& batch,bool final){
   if(!active())return false;
   if(batch.empty()||(!final&&batch.size()<16))return true;
   CacheAllocator allocator;JsonDocument doc(&allocator);
   JsonArray items=doc.to<JsonArray>();
   for(const auto& ep:batch){JsonObject entry=items.add<JsonObject>();entry["title"]=ep.title.c_str();entry["url"]=ep.url.c_str();entry["dur"]=ep.duration;}
   if(doc.overflowed())return false;
   size_t length=measureJson(doc);
   std::unique_ptr<char,decltype(&free)> encoded((char*)ps_malloc(length+2),free);
   if(!encoded)return false;
   if(serializeJson(doc,encoded.get(),length+1)!=length)return false;
   encoded.get()[length]='\n';
   if(batches.write((const uint8_t*)encoded.get(),length+1)!=length+1)return false;
   batches.flush();batch.clear();return true;
  };
  ok=radiofeed::fetchFileWithRetry(parser,problem,file,makeReader,active,pause,
    [&](size_t bytes,size_t items,bool parsing){
     rssBytes=bytes;rssItems=items;
     if(parsing&&!rssParsing.load()){rssDownloadMs=millis()-started;parseStarted=millis();}
     rssParsing=parsing;
     if(parsing)rssParseMs=millis()-parseStarted;else rssDownloadMs=millis()-started;
    },3,prepareRead,transfer.get(),16384,readSaved,saveBatch);
  if(rssParsing.load())rssParseMs=millis()-parseStarted;else rssDownloadMs=millis()-started;
  file.close();batches.close();
  if(!feedAlive(g))return;
  if(ok){
   // Rebuild only the final PSRAM list; the parser kept at most 16 episodes.
   batches=fs->open(batchPath,FILE_READ);
   if(!batches){feedFail(g,"SD RSS metadata reopen failed");return;}
   while(batches.available()){
    if(!active()){batches.close();return;}
    int ch=batches.peek();
    if(ch=='\n'||ch=='\r'||ch==' '||ch=='\t'){batches.read();continue;}
    CacheAllocator allocator;JsonDocument doc(&allocator);
    auto result=deserializeJson(doc,batches);
    if(result||!doc.is<JsonArray>()){batches.close();feedFail(g,"SD RSS metadata read failed");return;}
    for(JsonObject entry:doc.as<JsonArray>()){
     radiofeed::Episode ep;ep.title=entry["title"] | "";ep.url=entry["url"] | "";ep.duration=entry["dur"] | 0u;
     if(!radiofeed::urlOK(ep.url)){batches.close();feedFail(g,"SD RSS metadata invalid URL");return;}
     parser.items.push_back(std::move(ep));
    }
   }
   batches.close();
   if(parser.items.size()!=parser.itemCount){feedFail(g,"SD RSS metadata count mismatch");return;}
   if(!active())return;
   fs->remove(batchPath);
   if(fs->exists(saved)&&!fs->remove(saved)){feedFail(g,"SD RSS file replace failed");return;}
   if(!fs->rename(temporary,saved)){feedFail(g,"SD RSS file save failed");return;}
  }
 }else{
  // Preserve standalone playback without a card using the streaming parser.
  ok=radiofeed::fetchWithRetry(parser,problem,makeReader,active,pause,3,
    [&](size_t bytes,size_t items){rssBytes=bytes;rssItems=items;});
 }
 if(!feedAlive(g))return;
 if(!ok){feedFail(g,String("RSS fetch failed: ")+problem.c_str());return;}
 bool changed=true;
 {Guard l;if(g!=feedGeneration || url!=feedUrl)return;
  std::string current=selected>=0 && selected<(int)episodes.size()?std::string(episodes[selected].url.c_str()):std::string();
  auto summary=radiofeed::summarizeFetch(parser.items,episodes);
  auto merged=radiofeed::mergeCache(std::move(parser.items),episodes,current);
  changed=!radiofeed::sameCache(merged,episodes)||show!=String(parser.show.c_str())||cacheNote.length();
  int next=radiofeed::episodeIndex(merged,current);
  bool preserve=next>=0;
  episodes=std::move(merged);show=parser.show.c_str();selected=preserve?next:restoredSelection();
  // Reindex the current episode without touching its decoder, buffer or clock.
  if(!preserve)baseSec=resumePosition();
  refresh=false;feedError="";
  fetchDoneCount=summary.recent;fetchDoneNew=summary.added;fetchDoneUntil=millis()+5000;
  if(!want && (state=="loading" || (state=="error" && !error.length())))state="idle";
 }
 if(changed)cacheDirty=true;
 cacheService();
}

bool putSamples(uint32_t g,const int16_t* samples,uint32_t n){
 uint32_t off=0;
 while(off<n){
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
 std::unique_ptr<int16_t,decltype(&free)> monoBlock((int16_t*)ps_malloc(1024*sizeof(int16_t)),free);
 if(!decoder||!input||!pcm||!monoBlock){fail(g,"Not enough RAM for MP3 decoder");return;}
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
 bool done=false,decoded=false,needsInput=false;int16_t* mono=monoBlock.get();uint32_t count=0;
 uint32_t noAudioAt=millis();
 auto emit=[&](int16_t value){mono[count++]=value;if(count==1024){if(!putSamples(g,mono,count))return false;count=0;}return true;};
 while(alive(g)){
  // Do not wait on the network while complete MP3 frames are still buffered.
  if(radioNeedsInput(used,done,needsInput,DRMP3_MAX_FREE_FORMAT_FRAME_SIZE,[&](){return reader.buffered();})) {
   int n=reader.read(input.get()+used,32768-used);
   if(n<0){fail(g,reader.problem);return;}
   if(n)used+=n;else done=true;
  }
  if(!used&&done)break;
  drmp3dec_frame_info frame={};int samples=drmp3dec_decode_frame(decoder.get(),input.get(),used,pcm.get(),&frame);
  int consumed=frame.frame_bytes;needsInput=consumed<=0;
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

// Playback never waits for RSS downloads or SD metadata writes.
void worker(void*){
 uint32_t handled=0;
 for(;;){
  uint32_t g,pos;bool play;radiofeed::Episode ep;int kbps;
  {Guard l;g=generation;pos=baseSec;kbps=bitrate;
   play=enabled&&want&&selected>=0&&selected<(int)episodes.size();
   if(play)ep=episodes[selected];
  }
  if(g==handled||!play){vTaskDelay(pdMS_TO_TICKS(30));continue;}
  if(WiFi.status()!=WL_CONNECTED){
   {Guard l;if(g==generation)state="offline";}
   vTaskDelay(pdMS_TO_TICKS(100));continue;
  }
  handled=g;
  {Guard l;decoding=true;}
  playStream(g,ep,pos,kbps);
  {Guard l;decoding=false;}
 }
}
void feedWorker(void*){
 uint32_t handled=0;
 for(;;){
  bool importing;uint32_t importGen;String importUrl;
  {Guard l;importing=importPending;importPending=false;importGen=generation;importUrl=feedUrl;}
  if(importing){auto fs=sdFs();bool ok=fs&&cacheLoad(*fs,importGen,importUrl,true);
   if(ok){cacheDirty=true;cacheService();}else{Guard l;feedError="RSS import failed";refresh=false;}}
  cacheService();
  uint32_t g;bool fetch;String url;
  {Guard l;g=feedGeneration;fetch=enabled&&refresh;url=feedUrl;}
  if(!fetch || g==handled){vTaskDelay(pdMS_TO_TICKS(30));continue;}
  if(WiFi.status()!=WL_CONNECTED){vTaskDelay(pdMS_TO_TICKS(100));continue;}
  handled=g;fetchFeed(g,url);
 }
}
}

bool radioInit(){
 if(initialized)return true;
 mux=xSemaphoreCreateMutex();if(!mux)return false;
 ring=(int16_t*)ps_malloc(CAP*sizeof(int16_t));if(!ring)return false;
 prefs.begin("radio",false);feedUrl=prefs.getString("feed","");enabled=prefs.getBool("enabled",false)&&urlOK(feedUrl);
 lastEpisode=prefs.getString("last_episode","");
 size_t resumeBytes=prefs.getBytesLength("resume_v1");
 if(resumeBytes&&resumeBytes<=sizeof resumePoints&&resumeBytes%sizeof(ResumePoint)==0)
  prefs.getBytes("resume_v1",resumePoints,resumeBytes);
 for(const auto& point:resumePoints)resumeClock=std::max(resumeClock,point.used);
 refresh=enabled;state=enabled?"loading":"idle";
 TaskHandle_t playbackTask=nullptr;
 // lwIP TCP initial sequence generation requires an internal task stack.
 // Keep stacks bounded; decoder and TLS work buffers live in PSRAM.
 if(xTaskCreatePinnedToCore(worker,"rss-mp3",8192,nullptr,1,&playbackTask,0)!=pdPASS)return false;
 if(xTaskCreatePinnedToCore(feedWorker,"rss-feed",10240,nullptr,1,nullptr,0)!=pdPASS) {
  vTaskDelete(playbackTask);return false;
 }
 initialized=true;return true;
}
bool radioEnabled(){if(!initialized)return false;Guard l;return enabled;}
bool radioConfigure(const char* url,bool on){
 if(!initialized)return false;String value=url?url:"";value.trim();if((on||value.length())&&!urlOK(value))return false;
 {Guard l;saveResume();bool changed=value!=feedUrl;enabled=on;feedUrl=value;++generation;++feedGeneration;want=false;feedError="";fetchDoneUntil=0;
  if(changed){episodes.clear();selected=-1;show="";}
  cachePending=on&&episodes.empty();refresh=on&&episodes.empty();bitrate=0;baseSec=resumePosition();clearBuffer();error="";state=refresh?"loading":"idle";
 }
 prefs.putString("feed",value);prefs.putBool("enabled",on);return true;
}
void radioDisable(){if(!initialized)return;String saved;{Guard l;saved=feedUrl;}radioConfigure(saved.c_str(),false);}
void radioImport(){
 if(!initialized)return;Guard l;saveResume();++generation;++feedGeneration;want=false;clearBuffer();refresh=true;feedError="";importPending=true;
}
void radioRefresh(){
 if(!initialized)return;Guard l;if(!enabled || refresh)return;
 ++feedGeneration;cachePending=false;refresh=true;feedError="";fetchDoneUntil=0;
 if(!want && episodes.empty())state="loading";
}
bool radioRefreshing(){if(!initialized)return false;Guard l;return enabled&&refresh;}
void radioCancelRefresh(){
 if(!initialized)return;Guard l;if(!refresh)return;
 ++feedGeneration;refresh=false;cachePending=false;feedError="";fetchDoneUntil=0;
 if(state=="loading")state="idle";
}
void radioSelect(int i,bool play){if(!initialized)return;Guard l;if(i<0||i>=(int)episodes.size())return;
 saveResume();selected=i;saveSelection();++generation;want=play;baseSec=resumePosition();bitrate=0;audioStart=0;clearBuffer();state=play?"buffering":"paused";error="";}
void radioSkip(int delta){if(!initialized)return;int i,n;bool playing;{Guard l;i=selected;n=episodes.size();playing=want;}if(n)radioSelect((i+delta+n)%n,playing);}
void radioToggle(){if(!initialized)return;Guard l;if(!enabled||episodes.empty())return;
 if(want){saveResume();want=false;state="paused";}else{want=true;if(state=="error"||(!decoding&&!eof)){baseSec+=played/RATE;++generation;clearBuffer();}state=buffering?"buffering":"playing";}}
void radioPause(){if(!initialized)return;Guard l;saveResume();want=false;if(enabled)state="paused";}
static bool radioSeekLocked(uint32_t pos){
 if(!enabled||selected<0||selected>=(int)episodes.size())return false;
 if(episodes[selected].duration&&pos>=episodes[selected].duration)pos=episodes[selected].duration-1;
 baseSec=pos;++generation;clearBuffer();if(!want)saveResume();state=want?"buffering":"paused";error="";return true;
}
void radioSeek(int32_t delta){if(!initialized)return;Guard l;
 int64_t pos=(int64_t)baseSec+played/RATE+delta;if(pos<0)pos=0;
 radioSeekLocked((uint32_t)pos);}
bool radioSeekTo(uint32_t seconds){if(!initialized)return false;Guard l;return radioSeekLocked(seconds);}
bool radioPlaying(){if(!initialized)return false;Guard l;return enabled&&want;}
bool radioWantPlay(){if(!initialized)return false;Guard l;return enabled&&want&&!buffering&&fill>0;}
float radioBufferedSeconds(){if(!initialized)return 0;Guard l;return enabled?(float)fill/RATE:0.0f;}
uint32_t radioPull(int16_t* out,uint32_t maxSamples){
 if(!initialized)return 0;Guard l;if(!enabled||!want||buffering)return 0;
 if(!fill){if(!eof){buffering=true;state="buffering";++underruns;++streamUnderruns;
   prebufferTarget=std::min(RATE*16,prebufferTarget+RATE*2);}return 0;}
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
 if(refresh){
  char message[80];
  const char* format=rssParsing.load()?(langEn()?"Parsing %lu KB / %lu eps":"解析 %lu KB · %lu 集"):
     (langEn()?"Fetching %lu KB / %lu eps":"下载 %lu KB · %lu 集");
  snprintf(message,sizeof(message),format,
           (unsigned long)(rssBytes.load()/1024),(unsigned long)rssItems.load());
  status=message;
 }
 else if((int32_t)(fetchDoneUntil-millis())>0) {
  char message[64];
  snprintf(message,sizeof(message),langEn()?"Latest %u / New %u":"最近 %u 集 · 新增 %u 集",fetchDoneCount,fetchDoneNew);
  status=message;
 }
 if(feedError.length() && !want)status=feedError;
 if(error.length())status=error;copyText(info.show,sizeof info.show,status.c_str());return info;
}
void radioPoll(){
 if(!initialized)return;bool kick=false,next=false;int index=-1;
 {Guard l;if(!enabled)return;
  if(want&&buffering&&(fill>=prebufferTarget||(eof&&fill))){buffering=false;state="playing";}
  if(want&&(int32_t)(millis()-resumeSavedAt)>15000){saveResume();resumeSavedAt=millis();}
  kick=want&&!buffering&&fill>0;
  if(want&&eof&&!fill&&!episodes.empty()){next=true;index=(selected+1)%episodes.size();}
 }
 if(next)radioSelect(index,true);else if(kick&&!playerRunning())audioPlayerKick();
}
void radioStatus(JsonObject out){
 out["worker_stacks_psram"]=false;out["tls_all_psram"]=true;out["rss_received"]=rssBytes.load();out["rss_requests"]=rssRequests.load();out["rss_parsed"]=rssItems.load();out["rss_parsing"]=rssParsing.load();out["rss_file_first"]=true;out["rss_download_ms"]=rssDownloadMs.load();out["rss_parse_ms"]=rssParseMs.load();out["rss_sd_batch_bytes"]=16384;out["rss_metadata_psram"]=true;out["rss_parse_batch_episodes"]=16;out["rss_sd_read_internal"]=true;out["rss_read_internal_free"]=rssReadFree.load();out["rss_read_internal_largest"]=rssReadLargest.load();out["rss_http_status"]=rssHttpStatus.load();out["system_utc"]=(uint32_t)time(nullptr);out["clock_ready"]=time(nullptr)>1700000000;out["host_utc_sync"]=true;out["http_resume"]=true;out["rss_segment_bytes"]=0;out["rss_streaming"]=true;out["background_refresh"]=true;out["tls_ca_reattach"]=true;out["tls_psram"]=true;out["internal_free"]=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);out["internal_largest"]=heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);out["supported"]=true;out["cache_limit"]=nullptr;out["ready"]=initialized.load();if(!initialized)return;
 Guard l;out["cache_storage"]=cacheStorageReady;out["cache_error"]=cacheNote;out["enabled"]=enabled;out["feed"]=feedUrl;out["state"]=state;out["error"]=error.length()?error:feedError;out["refreshing"]=refresh;out["refresh_error"]=feedError;
 out["wifi"]=WiFi.status()==WL_CONNECTED;out["count"]=episodes.size();out["index"]=selected;
 out["pos"]=baseSec+played/RATE;out["buffer_s"]=(double)fill/RATE;out["underruns"]=underruns;
 out["buffer_capacity_s"]=CAP/RATE;out["prebuffer_s"]=prebufferTarget/RATE;out["stream_underruns"]=streamUnderruns;
 out["audio_bytes"]=audioNetBytes.load();out["audio_wait_max_ms"]=audioNetWaitMax.load();out["audio_reconnects"]=audioReconnects.load();out["rssi"]=WiFi.RSSI();

}
