"""Exercise the actual firmware HTTP body reader with interrupted responses."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class HttpReaderTests(unittest.TestCase):
    def test_segmented_feed_and_audio_resume(self):
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
int main(){
 radiohttp::Range r;
 assert(radiohttp::parseRange("bytes 65536-131071/1751572",r));
 assert(radiohttp::validRange(r,65536,1751572,65536,65536));
 assert(!radiohttp::validRange(r,0,1751572,65536,65536));
 assert(!radiohttp::validRange(r,65536,99,65536,65536));
 assert(!radiohttp::parseRange("bytes -1-3/9",r));
 assert(!radiohttp::parseRange("bytes 3-1/9",r));
 assert(!radiohttp::parseRange("bytes 0-9/9",r));
 assert(!radiohttp::parseRange("bytes 0-18446744073709551616/9",r));
 reset();responses={response("abc",3,206,"bytes 0-2/6"),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==2&&requests[0]["Range"]=="bytes=0-65535"&&requests[1]["Range"]=="bytes=3-65538");
 reset();responses={response("abc",3,206,"bytes 0-2/6"),response("",0,-5),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==3&&requests[1]["Range"]==requests[2]["Range"]);
 reset();responses={response("abc",6),response("def",3,206,"bytes 3-5/6")};
 {Reader reader(1);assert(reader.open("https://example.test/audio"));assert(drain(reader)=="abcdef");}
 assert(requests.size()==2&&requests[1]["Range"]=="bytes=3-"&&requests[1]["If-Range"]=="v1");
 reset();responses={response("ab",3,206,"bytes 0-2/6"),response("cdef",4,206,"bytes 2-5/6")};
 {Reader reader(1,true);assert(reader.open("https://example.test/rss"));assert(drain(reader)=="abcdef");}
 reset();responses={response("abc",6),response("abcdef",6)};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));assert(reader.read(b,32)==3);assert(reader.read(b,32)==-1);assert(reader.problem=="server ignored HTTP resume range");}
 reset();responses={response("abc",6),response("def",3,206,"bytes 3-5/6","v2")};
 {Reader reader(1);uint8_t b[32];assert(reader.open("https://example.test/audio"));assert(reader.read(b,32)==3);assert(reader.read(b,32)==-1);assert(reader.problem=="HTTP resource changed during transfer");}
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
            cpp.write_text('#include "http_reader_test_support.h"\n' + reader + checks)
            subprocess.run([compiler, "-std=c++11", "-I", str(root / "firmware/src"),
                            "-I", str(root / "firmware/tests"), str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
