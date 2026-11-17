"""Native checks for episode restoration and the page-number keypad."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class RadioNavigationTests(unittest.TestCase):
    def test_restore_and_jump(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler is not installed')
        root = pathlib.Path(__file__).resolve().parent.parent
        radio = (root / 'firmware/src/radio.cpp').read_text()
        pages = (root / 'firmware/src/pages.cpp').read_text()
        selection = radio[radio.index('int restoredSelection()'):radio.index('// Bounded NVS history')]
        navigation = pages[pages.index('static bool netList='):pages.index('static void radioButton(')]
        preamble = r'''
#include "rss_cache.h"
#include <algorithm>
#include <cassert>
using String=std::string;
std::vector<radiofeed::Episode> episodes;
String lastEpisode;
int selected=-1;
struct Prefs {int writes=0;String stored;void putString(const char*,const String& s){stored=s;writes++;}} prefs;
bool online=true;
bool radioEnabled(){return online;}
int radioCount(){return episodes.size();}
struct Info {int index;};
Info radioInfo(){return {selected};}
void radioSelect(int i,bool){selected=i;}
int playerCount(){return episodes.size();}
Info playerInfo(){return {selected};}
bool playerSelect(int i){selected=i;return true;}
bool playerDelete(int i){episodes.erase(episodes.begin()+i);return true;}
bool fetching=false;int fetches=0,cancels=0;
bool radioRefreshing(){return fetching;}
void radioRefresh(){fetching=true;fetches++;}
void radioCancelRefresh(){fetching=false;cancels++;}
const int RADIO_FOOTER_TOP=396;
'''
        checks = r'''
int main(){
 for(int i=0;i<1200;i++){radiofeed::Episode e;e.url="https://example.test/"+std::to_string(i);episodes.push_back(e);}
 selected=1103;saveSelection();assert(prefs.writes==1);
 saveSelection();assert(prefs.writes==1);
 lastEpisode=prefs.stored;selected=-1;assert(restoredSelection()==1103);
 std::reverse(episodes.begin(),episodes.end());assert(restoredSelection()==96);
 lastEpisode="missing";assert(restoredSelection()==0);
 selected=96;radioPageTap(350,430);assert(netList&&netListPage==24);
 radioPageTap(240,30);assert(netJump);
 radioPageTap(100,130); // 1
 radioPageTap(228,130); // 2
 assert(netJumpValue==12);
 radioListStep(1);assert(netListPage==24); // swipes do not change pages under keypad
 radioPageTap(390,430);assert(!netJump&&netListPage==11);
 radioPageTap(240,30);radioPageTap(390,430);assert(netJump); // zero rejected
 netJumpValue=301;radioPageTap(390,430);assert(netJump); // past last page rejected
 radioPageTap(240,430);assert(netJumpValue==30); // delete
 radioPageTap(390,430);assert(!netJump&&netListPage==29);
 radioPageTap(240,30);netJumpValue=2147483647;radioPageTap(100,130);
 assert(netJumpValue==2147483647); // no integer overflow
 radioPageTap(80,430);assert(!netJump&&netListPage==29);
 radioPageTap(100,90);assert(selected==116&&!netList);
 episodes.clear();netList=true;netJump=true;netJumpValue=1;
 radioPageTap(390,430);assert(!netJump&&netListPage==0);
 netList=false;
 radioPageTap(100,430);assert(fetching&&fetches==1&&cancels==0);
 radioPageTap(100,430);assert(!fetching&&fetches==1&&cancels==1);
 radioPageTap(100,430);assert(fetching&&fetches==2);
 online=false;for(int i=0;i<5;++i)episodes.push_back({});selected=0;
 radioPageTap(350,430);assert(netList&&netListPage==0);
 radioPageTap(400,90);assert(offlineDelete==0&&episodes.size()==5);
 radioPageTap(400,90);assert(offlineDelete==-1&&episodes.size()==4&&netList);
 radioPageTap(100,90);assert(selected==0&&!netList);
 radioPageTap(350,430);radioPageTap(400,90);radioListStep(1);assert(offlineDelete==-1);

}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / 'navigation.cpp'
            binary = str(pathlib.Path(tmp) / 'navigation')
            cpp.write_text(preamble + selection + navigation + checks)
            subprocess.run([compiler, '-std=c++11', '-I', str(root / 'firmware/src'), str(cpp), '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
