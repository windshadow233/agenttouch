// AgentTouch bounded RSS 2.0 parser: SAX-like, no whole-feed DOM.
#pragma once
#include <string>
#include "rss_text.h"
#include <vector>
#include <cctype>
#include <cstdlib>
#include <functional>

namespace radiofeed {
struct Episode { EpisodeText title, url; unsigned duration=0; };
inline void utf8(std::string& out, unsigned c) {
  if(c<128) out+=(char)c;
  else if(c<2048){out+=(char)(192|(c>>6));out+=(char)(128|(c&63));}
  else if(c<65536){out+=(char)(224|(c>>12));out+=(char)(128|((c>>6)&63));out+=(char)(128|(c&63));}
  else if(c<=0x10ffff){out+=(char)(240|(c>>18));out+=(char)(128|((c>>12)&63));out+=(char)(128|((c>>6)&63));out+=(char)(128|(c&63));}
}
inline std::string unescape(const std::string& s) {
  std::string o;
  for(size_t i=0;i<s.size();++i){
    if(s[i]!='&'){o+=s[i];continue;}
    size_t e=s.find(';',i+1); if(e==std::string::npos||e-i>12){o+='&';continue;}
    auto n=s.substr(i+1,e-i-1);
    if(n=="amp")o+='&';else if(n=="lt")o+='<';else if(n=="gt")o+='>';
    else if(n=="quot")o+='"';else if(n=="apos")o+='\'';
    else if(n.size()>1&&n[0]=='#'){
      char* end=nullptr;const char* p=n.c_str()+1;int base=10;
      if(*p=='x'||*p=='X'){++p;base=16;}
      unsigned long cp=strtoul(p,&end,base);
      if(end&&*end==0&&cp&&cp<=0x10ffff&&!(cp>=0xd800&&cp<=0xdfff))utf8(o,(unsigned)cp);
      else o+=s.substr(i,e-i+1);
    }else o+=s.substr(i,e-i+1);
    i=e;
  }
  return o;
}
inline bool urlOK(const std::string& u){return u.size()<=1023&&(u.rfind("https://",0)==0||u.rfind("http://",0)==0);}
inline std::string clipped(std::string s,size_t n){if(s.size()<=n)return s;while(n&&((unsigned char)s[n]&192)==128)--n;return s.substr(0,n);}
inline std::string trim(const std::string& s){size_t a=0,b=s.size();while(a<b&&isspace((unsigned char)s[a]))++a;while(b>a&&isspace((unsigned char)s[b-1]))--b;return s.substr(a,b-a);}
inline unsigned seconds(const std::string& text){unsigned value=0,part=0;for(char c:trim(text)){if(c==':'){value=value*60+part;part=0;}else if(c>='0'&&c<='9')part=part*10+(c-'0');else return 0;}return value*60+part;}
class Parser {
  enum State { TEXT,TAG,COMMENT,CDATA } state=TEXT;
  std::string tag,text,capture,tail; char quote=0;
  bool inItem=false,literal=false;Episode item;
  static std::string base(std::string n){size_t p=n.find(':');return p==std::string::npos?n:n.substr(p+1);}
  static std::string attr(const std::string& t,const std::string& key){
    size_t p=t.find_first_of(" \t\r\n");
    while(p!=std::string::npos&&p<t.size()){
      while(p<t.size()&&isspace((unsigned char)t[p]))++p;size_t a=p;
      while(p<t.size()&&!isspace((unsigned char)t[p])&&t[p]!='='&&t[p]!='/')++p;
      std::string name=t.substr(a,p-a);while(p<t.size()&&isspace((unsigned char)t[p]))++p;
      if(p==t.size()||t[p]!='='){if(p==a)++p;continue;}++p;
      while(p<t.size()&&isspace((unsigned char)t[p]))++p;if(p==t.size())break;
      char q=t[p++];if(q!='\''&&q!='"')continue;size_t e=t.find(q,p);if(e==std::string::npos)break;
      if(name==key)return unescape(t.substr(p,e-p));p=e+1;
    }return "";
  }
  void finishTag(){
    std::string t=trim(tag);if(t.empty()||t[0]=='?'||t[0]=='!')return;
    bool end=t[0]=='/';size_t a=end?1:0;size_t b=t.find_first_of(" \t\r\n/",a);
    std::string name=base(t.substr(a,b==std::string::npos?b:b-a));
    if(!end&&name=="item"){inItem=true;item={};capture.clear();text.clear();}
    else if(end&&name=="item"){
      if(inItem&&urlOK(item.url)){if(item.title.empty())item.title="Podcast";items.push_back(item);itemCount++;if(onBatch&&items.size()>=16&&!onBatch(items))invalid=true;}
      inItem=false;capture.clear();text.clear();
    }else if(!end&&inItem&&name=="enclosure"){
      auto u=attr(t,"url");auto type=attr(t,"type");
      for(char& c:type)c=(char)tolower((unsigned char)c);
      bool mp3=type.empty()||type=="audio/mpeg"||type=="audio/mp3"||type=="application/octet-stream";
      if(mp3&&urlOK(u))item.url=u;
    }else if(!end&&(name=="title"||name=="duration")&&capture.empty()){
      if((inItem&&name=="duration")||(name=="title"&&(inItem?item.title.empty():show.empty()))){capture=name;text.clear();literal=false;}
    }else if(end&&name==capture){
      auto value=trim(literal?text:unescape(text));
      if(name=="title"){if(inItem)item.title=clipped(value,191);else show=clipped(value,127);}
      else if(name=="duration")item.duration=seconds(value);
      capture.clear();text.clear();
    }
  }
public:
  std::vector<Episode> items;std::string show;bool invalid=false;size_t bytes=0,itemCount=0;
  std::function<bool(std::vector<Episode>&)> onBatch;
  void put(char c){
    if(invalid)return;if(++bytes>4*1024*1024){invalid=true;return;}
    if(state==COMMENT){tail+=c;if(tail.size()>3)tail.erase(0,1);if(tail=="-->"){state=TEXT;tail.clear();}return;}
    if(state==CDATA){tail+=c;if(tail.size()>=3){if(tail=="]]>"){state=TEXT;tail.clear();}else{if(!capture.empty()&&text.size()<4096)text+=tail[0];tail.erase(0,1);}}return;}
    if(state==TEXT){if(c=='<'){state=TAG;tag.clear();quote=0;}else if(!capture.empty()&&text.size()<4096)text+=c;return;}
    if(c=='>'&&!quote){finishTag();state=TEXT;tag.clear();return;}
    tag+=c;if(tag=="!--"){state=COMMENT;tail.clear();tag.clear();return;}
    if(tag=="![CDATA["){if(!capture.empty())literal=true;state=CDATA;tail.clear();tag.clear();return;}
    if(c=='\''||c=='"'){if(!quote)quote=c;else if(quote==c)quote=0;}
    if(tag.size()>8192){invalid=true;tag.clear();}
  }
  void write(const char* data,size_t n){for(size_t i=0;i<n;++i)put(data[i]);}
};
}
