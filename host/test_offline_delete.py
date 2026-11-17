"""Exercise the firmware deletion routine with a simulated SD card/audio task."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class OfflineDeleteTests(unittest.TestCase):
    def test_delete_and_playback_state(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler unavailable')
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / 'firmware/src/player.cpp').read_text()
        deletion = source[source.index('bool playerDelete('):source.index('bool playerDeleteName(')]
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <cstring>
struct Item {char name[48];};Item s_items[8];
int s_n=0,s_cur=-1,s_openIdx=0,s_seekGen=0;unsigned s_blk=0;
bool s_want=false,s_running=false,stuck=false;int kicks=0;
struct {bool hasItems;} gInfo;
struct Handle {operator bool(){return true;}void close(){}} s_file;
const char* PL_STATE="/state";
struct FS {
 std::set<std::string> files;bool fail=false;
 bool exists(const char* p){return files.count(p);}
 bool remove(const char* p){if(fail)return false;return files.erase(p);}
} disk;
FS* sdFs(){return &disk;}bool radioEnabled(){return false;}
bool plSuspend(){bool was=s_want;s_want=false;s_running=stuck;return was;}
void plPath(char* out,size_t n,const char* name,const char* ext){snprintf(out,n,"/%s%s",name,ext);}
void plScan(){s_n=0;for(const char* name:{"a","b","c"})if(disk.files.count(std::string("/")+name+".ima")&&disk.files.count(std::string("/")+name+".json"))snprintf(s_items[s_n++].name,48,"%s",name);}
int plFind(const char* name){for(int i=0;i<s_n;i++)if(!strcmp(name,s_items[i].name))return i;return -1;}
bool deleteRecord(const char*){return true;}
void plLoadMeta(){}void playerSaveState(){}void audioPlayerKick(){kicks++;}
void reset(){disk.files={"/a.ima","/a.json","/b.ima","/b.json","/c.ima","/c.json"};disk.fail=false;stuck=false;s_running=false;s_want=true;kicks=0;plScan();s_cur=1;s_blk=42;}
'''
        checks = r'''
int main(){
 reset();assert(playerDelete(0));assert(s_n==2&&s_cur==0&&s_blk==42&&s_want&&kicks==1);assert(!disk.exists("/a.ima")&&!disk.exists("/a.json"));
 reset();assert(playerDelete(1));assert(s_n==2&&s_blk==0&&!s_want&&kicks==0);assert(!disk.exists("/b.ima")&&!disk.exists("/b.json"));
 reset();stuck=true;assert(!playerDelete(1));assert(s_n==3&&s_want&&disk.exists("/b.ima"));
 reset();disk.fail=true;assert(!playerDelete(1));assert(s_n==3&&s_blk==42&&s_want);
 reset();assert(!playerDelete(-1)&&!playerDelete(3));assert(s_n==3);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = pathlib.Path(directory) / 'delete.cpp'
            binary = pathlib.Path(directory) / 'delete'
            cpp.write_text(harness + deletion + checks)
            subprocess.run([compiler, '-std=c++11', str(cpp), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
