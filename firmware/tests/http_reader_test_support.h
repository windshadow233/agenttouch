#include <string>
#include <map>
#include <deque>
#include <vector>
#include <algorithm>
#include <type_traits>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <atomic>
std::atomic<uint32_t> audioNetBytes{0},audioNetWaitMax{0},audioReconnects{0};
std::atomic<uint32_t> rssBytes{0},rssRequests{0};std::atomic<int> rssHttpStatus{0};
#include "http_range.h"
class String:public std::string {
public:
 using std::string::string;
 String(){} String(const std::string& s):std::string(s){}
 template<class T,typename std::enable_if<std::is_integral<T>::value,int>::type=0>
 String(T v):std::string(std::to_string(v)){}
 bool startsWith(const char* s)const{return compare(0,strlen(s),s)==0;}
 int indexOf(const char* s)const{auto i=find(s);return i==npos?-1:(int)i;}
 void toLowerCase(){for(char& c:*this)c=std::tolower((unsigned char)c);}
};
const int WL_CONNECTED=3,HTTPC_STRICT_FOLLOW_REDIRECTS=1;
bool nullStream=false;
bool running=true;bool alive(uint32_t){return running;}bool feedAlive(uint32_t){return running;}
bool urlOK(const String& s){return s.startsWith("https://");}
struct {int status(){return WL_CONNECTED;}}WiFi;
struct WiFiClient{};
struct RadioSecureClient:WiFiClient {String diagnostic;int lastError(char*,size_t){return 0;}void setCACertBundle(const uint8_t*,size_t){}void setHandshakeTimeout(int){}};
uint8_t radioCaStart[1]={},radioCaEnd[1]={};
uint32_t tick=0;uint32_t millis(){return tick++;}void vTaskDelay(uint32_t ms){tick+=ms;}
#define pdMS_TO_TICKS(x) (x)
struct {template<class... T>void printf(const char*,T...){}}Serial;
struct Response {int status=200,size=0;String body;std::map<String,String> headers;};
std::deque<Response> responses;
std::vector<std::map<String,String>> requests;
struct Stream {
 String body;size_t offset=0;
 int available(){return body.size()-offset;}
 int read(uint8_t* out,size_t n){n=std::min(n,body.size()-offset);memcpy(out,body.data()+offset,n);offset+=n;return n;}
 bool connected(){return offset<body.size();}
};
struct HTTPClient {
 Response response;Stream stream;std::map<String,String> sent;
 void end(){sent.clear();}
 void setAcceptEncoding(const char*){}
 void setConnectTimeout(int){}void setTimeout(int){}void setReuse(bool){}void useHTTP10(bool){}
 void setFollowRedirects(int){}void setRedirectLimit(int){}void setUserAgent(const char*){}
 bool begin(WiFiClient&,const String&){return true;}
 void collectHeaders(const char**,int){}
 void addHeader(const char* k,const String& v){sent[k]=v;}
 int GET(){assert(!responses.empty());requests.push_back(sent);response=responses.front();responses.pop_front();stream.body=response.body;stream.offset=0;return response.status;}
 String header(const char* k){return response.headers[k];}
 int getSize(){return response.size;}
 Stream* getStreamPtr(){return nullStream?nullptr:&stream;}
 static String errorToString(int){return "connection failed";}
};
static Response response(const char* body,int size,int status=200,const char* range="",const char* etag="v1"){
 Response r;r.body=body;r.size=size;r.status=status;r.headers["ETag"]=etag;r.headers["Content-Range"]=range;return r;
}
static void reset(){responses.clear();requests.clear();running=true;nullStream=false;}
