"""Exercise the actual firmware HTTP body reader with interrupted responses."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class HttpReaderTests(unittest.TestCase):
    def test_streaming_feed_and_audio_resume(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/radio.cpp").read_text()
        reader = source[source.index("class Reader {"):source.index("\nbool clockReady(")]
        checks = r'''
static std::string drain(Reader& reader){
 uint8_t buf[32];std::string result;int n;
 while((n=reader.read(buf,sizeof buf))>0)result.append((char*)buf,n);
 assert(n==0);return result;
}
struct MemoryFile {
 std::string data;size_t offset=0;bool failWrite=false,failRead=false;int transientReads=0;
 size_t write(const uint8_t* b,size_t n){if(failWrite)return 0;data.append((const char*)b,n);return n;}
 void flush(){}
 bool seek(size_t n){offset=n;return n<=data.size();}
 int read(uint8_t* b,size_t n){if(failRead)return -1;if(transientReads>0){transientReads--;return 0;}n=std::min(n,data.size()-offset);memcpy(b,data.data()+offset,n);offset+=n;return n;}
};
struct Feed:Reader {
 Feed():Reader(1,true){}
 bool open(){return Reader::open("https://example.test/rss");}
};
static void checkFileFetch(){
 reset();
 std::string first="<rss><channel><item><title>Before disconnect</title><encl";
 std::string rest="osure url='https://example.test/1.mp3'/></item></channel></rss>";
 std::string body=first+rest;
 std::string range="bytes "+std::to_string(first.size())+"-"+std::to_string(body.size()-1)+"/"+std::to_string(body.size());
 responses={response(first.c_str(),body.size()),response("",0,-5),response("",0,-5),response("",0,-5),response(rest.c_str(),rest.size(),206,range.c_str())};
 MemoryFile file;radiofeed::Parser parser;std::string error;size_t previous=0;int pauses=0;bool sawParsing=false;
 assert(radiofeed::fetchFileWithRetry(parser,error,file,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[&](){pauses++;},
  [&](size_t bytes,size_t items,bool parsing){
   assert(bytes>=previous);previous=bytes;
   if(parsing){assert(file.data==body);sawParsing=true;}else assert(items==0);
  }));
 assert(pauses==1&&sawParsing&&file.data==body&&parser.items.size()==1);
 assert(parser.items[0].title=="Before disconnect");
 assert(requests.size()==5&&!requests[0].count("Range"));
 for(size_t i=1;i<requests.size();i++)assert(requests[i]["Range"]=="bytes="+std::to_string(first.size())+"-");
 reset();responses={response(first.c_str(),body.size(),200,"","")};
 MemoryFile noValidator;radiofeed::Parser unchanged;unchanged.show="old";
 assert(!radiofeed::fetchFileWithRetry(unchanged,error,noValidator,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){},[](size_t,size_t,bool){}));
 assert(requests.size()==1&&noValidator.data==first&&unchanged.show=="old"&&unchanged.items.empty());
 assert(error=="RSS server has no resume validator; tap Fetch to restart");
 reset();responses={response(body.c_str(),body.size())};
 MemoryFile fullDisk;fullDisk.failWrite=true;
 assert(!radiofeed::fetchFileWithRetry(unchanged,error,fullDisk,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){},[](size_t,size_t,bool){}));
 assert(error=="SD RSS file write failed"&&unchanged.show=="old");
 reset();responses={response(body.c_str(),body.size())};
 MemoryFile recovered;recovered.transientReads=1;int reopens=0;uint8_t batch[16384];
 assert(radiofeed::fetchFileWithRetry(unchanged,error,recovered,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){},[](size_t,size_t,bool){},3,
  [&](MemoryFile& f,size_t offset){reopens++;return f.seek(offset);},batch,sizeof batch));
 assert(reopens==2&&requests.size()==1&&recovered.data==body&&unchanged.items.size()==1);
 reset();responses={response(body.c_str(),body.size())};
 MemoryFile badRead;badRead.failRead=true;
 assert(!radiofeed::fetchFileWithRetry(unchanged,error,badRead,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){},[](size_t,size_t,bool){}));
 assert(error.find("SD RSS file read failed at 0/")==0&&badRead.data==body);
}
static void checkFetchCancellation(){
 const char* body="<rss><channel><item><title>One</title><enclosure url='https://example.test/1.mp3'/></item></channel></rss>";
 reset();responses={response(body,strlen(body))};
 MemoryFile file;radiofeed::Parser old;old.show="keep old list";std::string error;
 assert(!radiofeed::fetchFileWithRetry(old,error,file,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){assert(false);},
  [](size_t bytes,size_t,bool parsing){if(bytes&&!parsing)running=false;}));
 assert(requests.size()==1&&old.show=="keep old list"&&old.items.empty());
 reset();responses={response(body,strlen(body))};
 MemoryFile parsed;
 assert(!radiofeed::fetchFileWithRetry(old,error,parsed,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){assert(false);},
  [](size_t,size_t,bool parsing){if(parsing)running=false;}));
 assert(requests.size()==1&&old.show=="keep old list"&&old.items.empty());
}
static void checkBoundedBatches(){
 reset();std::string body="<rss><channel><title>Batch show</title>";
 for(int i=0;i<500;i++)body+="<item><title>Episode "+std::to_string(i)+"</title><enclosure url='https://example.test/"+std::to_string(i)+".mp3'/></item>";
 body+="</channel></rss>";
 responses={response(body.c_str(),body.size())};
 MemoryFile file;radiofeed::Parser parser;std::string error;size_t saved=0,writes=0,progress=0;
 assert(radiofeed::fetchFileWithRetry(parser,error,file,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){},
  [&](size_t,size_t count,bool parsing){if(parsing){assert(count>=progress);progress=count;}},3,
  radiofeed::SeekSavedFile{},nullptr,0,radiofeed::ReadSavedFile{},
  [&](std::vector<radiofeed::Episode>& batch,bool final){
   assert(batch.size()<=16);if(!final)assert(batch.size()==16);
   saved+=batch.size();writes++;batch.clear();return true;
  }));
 assert(saved==500&&writes==32&&progress==500&&parser.itemCount==500&&parser.items.empty());
 assert(parser.show=="Batch show"&&!parser.onBatch);
 reset();responses={response(body.c_str(),body.size())};
 MemoryFile failed;radiofeed::Parser old;old.show="old";
 assert(!radiofeed::fetchFileWithRetry(old,error,failed,
  [](){return std::unique_ptr<Feed>(new Feed);},[](){return running;},[](){},[](size_t,size_t,bool){},3,
  radiofeed::SeekSavedFile{},nullptr,0,radiofeed::ReadSavedFile{},
  [](std::vector<radiofeed::Episode>&,bool){return false;}));
 assert(error=="SD RSS metadata batch write failed"&&old.show=="old"&&old.items.empty());
}
int main(){
 checkFileFetch();checkBoundedBatches();checkFetchCancellation();
 radiohttp::Range r;
 assert(radiohttp::parseRange("bytes 65536-131071/1751572",r));
 assert(radiohttp::validRange(r,65536,1751572,65536,65536));
 assert(!radiohttp::validRange(r,0,1751572,65536,65536));
 assert(!radiohttp::validRange(r,65536,99,65536,65536));
 assert(!radiohttp::parseRange("bytes -1-3/9",r));
 assert(!radiohttp::parseRange("bytes 3-1/9",r));
 assert(!radiohttp::parseRange("bytes 0-9/9",r));
 assert(!radiohttp::parseRange("bytes 0-18446744073709551616/9",r));
 reset();{
  std::string body(200000,'x');responses={response(body.c_str(),body.size())};
  Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)==body);
  assert(requests.size()==1&&!requests[0].count("Range")&&rssBytes==body.size());
 }
 reset();responses={response("abc",6),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==2&&!requests[0].count("Range")&&requests[1]["Range"]=="bytes=3-"&&requests[1]["If-Range"]=="v1");
 reset();responses={response("abc",3,206,"bytes 0-2/6"),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==2&&!requests[0].count("Range")&&requests[1]["Range"]=="bytes=3-");
 reset();responses={response("abc",3,206,"bytes 0-2/6"),response("",0,-5),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==3&&requests[1]["Range"]==requests[2]["Range"]);
 reset();responses={response("abc",6),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1);assert(reader.open("https://example.test/audio"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==2&&requests[1]["Range"]=="bytes=3-"&&requests[1]["If-Range"]=="v1");
 reset();responses={response("ab",3,206,"bytes 0-2/6"),response("cdef",4,206,"bytes 2-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 reset();responses={response("abc",6),response("abcdef",6)};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));assert(reader.read(b,32)==3);assert(reader.read(b,32)==-1);assert(reader.problem=="server ignored HTTP resume range; tap Fetch to restart");}
 reset();responses={response("abc",6),response("def",3,206,"bytes 3-5/6","v2")};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));assert(reader.read(b,32)==3);assert(reader.read(b,32)==-1);assert(reader.problem=="HTTP resource changed during transfer; tap Fetch to restart");}
 reset();responses={response("abc",6),response("def",3,206,"bytes 2-4/6")};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));assert(reader.read(b,32)==3);assert(reader.read(b,32)==-1);assert(reader.problem=="invalid HTTP resume range");}
 reset();{auto chunked=response("3\r\nabc\r\n0\r\n\r\n",-1);chunked.headers["Transfer-Encoding"]="chunked";responses={chunked};}
 {Reader reader(1);assert(reader.open("https://example.test/audio"));assert(drain(reader)=="abc");}
 reset();{auto chunked=response("3\r\nab",-1);chunked.headers["Transfer-Encoding"]="chunked";responses={chunked,response("c",1,206,"bytes 2-2/3")};}
 {Reader reader(1);assert(reader.open("https://example.test/audio"));assert(drain(reader)=="abc");}
 reset();responses={response("",6,200,"","")};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));
  nullStream=true;assert(reader.read(b,32)==-1);assert(reader.problem.length());nullStream=false;}
 reset();responses={response("abc",6)};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));running=false;assert(reader.read(b,32)==-1);assert(requests.size()==1);}
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "reader.cpp"
            binary = str(pathlib.Path(tmp) / "reader-test")
            cpp.write_text('#include "http_reader_test_support.h"\n#include "rss_fetch.h"\n#include <memory>\n' + reader + checks)
            subprocess.run([compiler, "-std=c++11", "-I", str(root / "firmware/src"),
                            "-I", str(root / "firmware/tests"), str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
