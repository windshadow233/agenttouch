"""Check UTC/local conversion used by RTC boot and host time pushes."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class BoardClockTests(unittest.TestCase):
    def test_system_clock_uses_utc(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/boardrtc.cpp").read_text()
        functions = source[source.index("static bool systemUtcSet("):source.index("\nbool rtcInit()")]
        preamble = r'''
#include <cstdint>
#include <ctime>
#include <sys/time.h>
#include <cassert>
#include <map>
#include <string>
struct Prefs {
 std::map<std::string,int32_t> values;int writes=0;
 bool isKey(const char* k){return values.count(k);}
 int32_t getInt(const char* k,int32_t fallback){return values.count(k)?values[k]:fallback;}
 void putInt(const char* k,int32_t v){values[k]=v;writes++;}
}clockPrefs;
bool clockPrefsOpen=true;
time_t systemUtc=0;uint32_t displayed=0,rtcLocal=0;
time_t fakeTime(time_t*){return systemUtc;}
int fakeSetTime(const timeval* tv,void*){systemUtc=tv->tv_sec;return 0;}
void clockSet(uint32_t local){displayed=local;}
void rtcHostSync(uint32_t local){rtcLocal=local;}
#define time fakeTime
#define settimeofday fakeSetTime
'''
        checks = r'''
int main(){
 const uint32_t utc=1794591000;
 rtcApplyHostTime(utc,8*3600);
 assert(systemUtc==utc&&displayed==utc+8*3600&&rtcLocal==displayed);
 assert(clockPrefs.values["utc_off"]==8*3600&&clockPrefs.writes==1);
 rtcApplyHostTime(utc+60,8*3600);assert(clockPrefs.writes==1);
 systemUtc=0;systemUtcFromRtc(utc+8*3600);assert(systemUtc==utc);
 systemUtc=utc+100;systemUtcFromRtc(utc+8*3600);assert(systemUtc==utc+100);
 rtcApplyHostTime(utc,-5*3600);assert(systemUtc==utc&&displayed==utc-5*3600);
 systemUtc=0;systemUtcFromRtc(utc-5*3600);assert(systemUtc==utc);
 clockPrefs.values.clear();systemUtc=0;systemUtcFromRtc(utc);assert(systemUtc==0);
 rtcApplyHostTime(100,0);assert(systemUtc==0); // never accept a reset/default date
 rtcApplyHostTime(utc,15*3600);assert(systemUtc==0);
 clockPrefsOpen=false;rtcApplyHostTime(utc,0);assert(systemUtc==utc&&displayed==utc);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "clock.cpp"
            binary = str(pathlib.Path(tmp) / "clock-test")
            cpp.write_text(preamble + functions + checks)
            subprocess.run([compiler, "-std=c++11", str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
