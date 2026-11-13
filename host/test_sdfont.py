"""Native SD glyph-cache regression tests with short and failed card reads."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class SdFontTests(unittest.TestCase):
    def test_cache_recovery(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/sdfont.cpp").read_text()
        source = "\n".join(line for line in source.splitlines()
                           if not line.startswith("#include"))
        preamble = r'''
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <cassert>
enum {SDF_HEAD,SDF_BODY,SDF_QUOT,SDF_TINY,SDF_N};
const int MALLOC_CAP_SPIRAM=1;
bool failPool=true,failRead=false;uint32_t ticks=0;
void* heap_caps_calloc(size_t n,size_t s,int){return calloc(n,s);}
void* heap_caps_malloc(size_t n,int){
 if(n==512*512&&failPool){failPool=false;return nullptr;}return malloc(n);
}
uint32_t millis(){return ticks;}
struct {template<class... T>void printf(const char*,T...){}}Serial;
std::map<std::string,std::vector<uint8_t>> files;
struct File {
 std::vector<uint8_t>* data=nullptr;size_t pos=0;
 explicit operator bool()const{return data;}
 void close(){data=nullptr;}
 size_t size(){return data?data->size():0;}
 size_t position(){return pos;}
 bool seek(size_t n){if(!data||n>data->size())return false;pos=n;return true;}
 int read(uint8_t* out,size_t n){
  if(!data||(failRead&&pos>=37))return 0;
  n=std::min(n,std::min((size_t)2,data->size()-pos));
  memcpy(out,data->data()+pos,n);pos+=n;return n;
 }
};
const char* FILE_READ="r";
namespace fs {struct FS{
 File open(const char* path,const char*){
  File f;auto it=files.find(path);if(it!=files.end())f.data=&it->second;return f;
 }
};}
fs::FS filesystem;fs::FS* sdFs(){return &filesystem;}
'''
        checks = r'''
void u32(std::vector<uint8_t>& b,uint32_t v){for(int i=0;i<4;i++)b.push_back((v>>(8*i))&255);}
int main(){
 std::vector<uint8_t> data={'A','F','N','1',18,0};u32(data,2);
 u32(data,0x4e00);u32(data,0);u32(data,0x5000);u32(data,6);
 for(uint8_t v:{1,1,18,0,0,240,1,1,19,0,0,128})data.push_back(v);
 files["/agentpet/fonts/tiny18.afn"]=data;
 sdFontLoadAll();assert(!sdFontReady(SDF_TINY)); // pool allocation failed
 ticks=2500;sdFontPoll();assert(sdFontReady(SDF_TINY));
 const uint8_t* first=sdFontGlyph(SDF_TINY,0x4e00);assert(first&&first[2]==18&&first[5]==240);
 // These codepoints share a cache slot. Fail after overwriting its header.
 failRead=true;assert(!sdFontGlyph(SDF_TINY,0x5000));
 assert(!sdFontReady(SDF_TINY));
 assert(!sdFontGlyph(SDF_TINY,0x4e00)); // must not return the overwritten old record
 failRead=false;ticks=5000;sdFontPoll();assert(sdFontReady(SDF_TINY));
 first=sdFontGlyph(SDF_TINY,0x4e00);assert(first&&first[2]==18&&first[5]==240);
 const uint8_t* second=sdFontGlyph(SDF_TINY,0x5000);assert(second&&second[2]==19&&second[5]==128);
 assert(!sdFontGlyph(SDF_TINY,0x9999));
 files["/agentpet/fonts/tiny18.afn"].resize(12);sdFontLoadAll();
 assert(!sdFontReady(SDF_TINY)); // truncated index cannot be marked ready
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "sdfont.cpp"
            binary = str(pathlib.Path(tmp) / "sdfont-test")
            cpp.write_text(preamble + source + checks)
            subprocess.run([compiler, "-std=c++11", str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
