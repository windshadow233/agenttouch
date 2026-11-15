#include "rss_fetch.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>

struct Attempt {
  std::string body;
  int status=200;
  bool interrupted=false;
};
struct Plan {
  std::vector<Attempt> attempts;
  unsigned opens=0,pauses=0,live=0;
  std::vector<size_t> offsets;
  bool active=true,cancelOnRead=false;
};
struct Reader {
  Plan& plan;Attempt attempt;size_t offset=0;
  std::string problem;int status=0;
  explicit Reader(Plan& p):plan(p) {assert(!p.live);p.live++;}
  ~Reader(){plan.live--;}
  size_t delivered=0;
  bool retryable()const{return !(status>=400&&status<500&&status!=408&&status!=429);}
  bool resume(){return open();}
  bool open(){
    plan.offsets.push_back(delivered);offset=0;
    assert(plan.opens<plan.attempts.size());attempt=plan.attempts[plan.opens++];
    status=attempt.status;problem="HTTP "+std::to_string(status);return status==200;
  }
  int read(uint8_t* out,size_t n){
    if(plan.cancelOnRead){plan.active=false;problem="cancelled";return -1;}
    if(offset==attempt.body.size()) {
      if(attempt.interrupted){problem="HTTP body ended early";return -1;}
      return 0;
    }
    size_t take=std::min(n,attempt.body.size()-offset);
    memcpy(out,attempt.body.data()+offset,take);offset+=take;delivered+=take;return take;
  }
};
static Attempt response(const std::string& body,int status=200,bool interrupted=false){
  Attempt a;a.body=body;a.status=status;a.interrupted=interrupted;return a;
}
static bool fetch(Plan& plan,radiofeed::Parser& parser,std::string& error){
  return radiofeed::fetchWithRetry(parser,error,
    [&](){return std::unique_ptr<Reader>(new Reader(plan));},
    [&](){return plan.active;},[&](){assert(plan.live==1);plan.pauses++;});
}
int main(){
  std::string item="<item><title>One</title><enclosure url='https://example.test/1.mp3' type='audio/mpeg'/></item>";
  std::string second="<item><title>Two</title><enclosure url='https://example.test/2.mp3' type='audio/mpeg'/></item>";
  std::string full="<rss><channel><title>Show</title>"+item+second+"</channel></rss>";
  {
    std::string large="<rss><channel>";
    for(int i=0;i<1200;i++)large+="<item><title>Episode</title><enclosure url='https://example.test/"+std::to_string(i)+".mp3'/></item>";
    large+="</channel></rss>";
    Plan p;p.attempts={response(large)};radiofeed::Parser parser;std::string error;
    assert(fetch(p,parser,error));assert(parser.items.size()==1200);
    auto merged=radiofeed::mergeCache(parser.items,{parser.items.back()});
    assert(merged.size()==1200&&merged.back().url==parser.items.back().url);
    auto summary=radiofeed::summarizeFetch(parser.items,{});
    assert(summary.recent==1200&&summary.added==1200);
  }
  {
    Plan p;p.attempts={response(item,200,true),response(second+"</channel></rss>")};
    radiofeed::Parser parser;std::string error;
    std::vector<std::pair<size_t,size_t>> progress;
    bool ok=radiofeed::fetchWithRetry(parser,error,
      [&](){return std::unique_ptr<Reader>(new Reader(p));},
      [&](){return p.active;},[&](){p.pauses++;},3,
      [&](size_t bytes,size_t items){progress.push_back({bytes,items});});
    assert(ok&&progress.size()==3);
    assert(progress[0]==std::make_pair(size_t(0),size_t(0)));
    assert(progress[1]==std::make_pair(item.size(),size_t(1)));
    assert(progress.back()==std::make_pair(item.size()+second.size()+size_t(16),size_t(2)));
  }
  {
    Plan p;p.attempts={response(item,200,true),response(second+"</channel></rss>")};
    radiofeed::Parser parser;std::string error;
    assert(fetch(p,parser,error));assert(p.opens==2&&p.pauses==1&&!p.live);
    assert(p.offsets==std::vector<size_t>({0,item.size()}));
    assert(parser.items.size()==2&&parser.items[0].title=="One"&&error.empty());
  }
  {
    Plan p;p.attempts={response(item,200,true),response(item,200,true),response(item,200,true)};
    radiofeed::Parser parser;parser.show="existing";std::string error;
    assert(!fetch(p,parser,error));assert(p.opens==3&&p.pauses==2);
    assert(parser.show=="existing"&&parser.items.empty()&&error=="HTTP body ended early");
  }
  {
    Plan p;p.attempts={response("",404)};radiofeed::Parser parser;std::string error;
    assert(!fetch(p,parser,error));assert(p.opens==1&&p.pauses==0&&error=="HTTP 404");
  }
  {
    Plan p;p.attempts={response("",503),response(full)};radiofeed::Parser parser;std::string error;
    assert(fetch(p,parser,error));assert(p.opens==2);
  }
  {
    Plan p;p.attempts={response("<rss><channel/></rss>")};radiofeed::Parser parser;std::string error;
    assert(!fetch(p,parser,error));assert(p.opens==1&&error=="RSS has no audio enclosure items");
  }
  {
    Plan p;p.attempts={response("<"+std::string(8200,'x'))};radiofeed::Parser parser;std::string error;
    assert(!fetch(p,parser,error));assert(p.opens==1&&error=="RSS is too large or malformed");
  }
  {
    Plan p;p.cancelOnRead=true;p.attempts={response(full)};radiofeed::Parser parser;std::string error;
    assert(!fetch(p,parser,error));assert(p.opens==1&&p.pauses==0&&!p.live);
  }
}
