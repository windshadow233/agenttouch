"""Verify decoder read scheduling and actual PCM underrun/rebuffer logic."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class RadioBufferingTests(unittest.TestCase):
    def test_bounded_buffer_and_network_read_policy(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/radio.cpp").read_text()
        start = source.index("constexpr uint32_t RATE=")
        policy = source[start:source.index("SemaphoreHandle_t mux", start)]
        start = source.index("float radioBufferedSeconds()")
        pull = source[start:source.index("int radioCount()", start)]
        start = source.index("void radioPoll()")
        poll = source[start:source.index("void radioStatus(", start)]
        preamble = r'''
#include "radio_dsp.h"
#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cassert>
struct Guard{};
bool initialized=true,enabled=true,want=true,buffering=false,eof=false;
uint32_t head=0,tail=0,fill=0,played=0,underruns=0,resumeSavedAt=0;
uint32_t millis(){return 20000;}
int saves=0;void saveResume(){saves++;}
std::string state="playing";
std::vector<int> episodes{1};int selected=0;
void radioSelect(int,bool){} bool playerRunning(){return true;}void audioPlayerKick(){}
'''
        checks = r'''
int main(){
 int probes=0;
 auto absent=[&](){probes++;return false;};
 auto ready=[&](){probes++;return true;};
 assert(!radioNeedsInput(8192,false,false,2304,absent)); // decode buffered frames instead of waiting
 assert(radioNeedsInput(8192,false,false,2304,ready));
 int before=probes;
 assert(radioNeedsInput(10,false,false,2304,absent));
 assert(radioNeedsInput(8192,false,true,2304,absent));
 assert(!radioNeedsInput(24000,false,false,2304,ready));
 assert(!radioNeedsInput(0,true,true,2304,ready));assert(probes==before);
 assert(CAP==RATE*30&&PREBUFFER==RATE*8);
 int16_t out[256];assert(radioBufferedSeconds()==0);assert(radioPull(out,256)==0);
 assert(buffering&&want&&underruns==1&&streamUnderruns==1&&prebufferTarget==RATE*10);
 fill=RATE*9;radioPoll();assert(buffering&&saves==1);
 radioPoll();assert(saves==1);
 fill=RATE*10;assert(radioBufferedSeconds()==10.0f);radioPoll();assert(!buffering&&state=="playing");
 tail=CAP-128;fill=256;
 for(int i=0;i<128;i++){ring[CAP-128+i]=i;ring[i]=i+128;}
 assert(radioPull(out,256)==256&&tail==128&&fill==0);
 for(int i=0;i<256;i++)assert(out[i]==i);
 for(int i=0;i<10;i++){buffering=false;radioPull(out,256);}
 assert(prebufferTarget==RATE*16); // recovery target remains bounded
 eof=true;fill=256;buffering=true;radioPoll();assert(!buffering); // drain a short final segment
 want=false;uint32_t pending=fill;assert(radioPull(out,256)==0&&fill==pending);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "buffering.cpp"
            binary = str(pathlib.Path(tmp) / "buffering-test")
            cpp.write_text(preamble + policy + "\nstd::vector<int16_t> storage(CAP);int16_t* ring=storage.data();\n" + pull + poll + checks)
            subprocess.run([compiler, "-std=c++11", "-I", str(root / "firmware/src"),
                            str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
