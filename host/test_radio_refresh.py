"""Compile the firmware's control functions to check background refresh state."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class RadioRefreshTests(unittest.TestCase):
    def test_refresh_preserves_playback(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/radio.cpp").read_text()
        ranges = [
            ("void feedFail(", "void fail("),
            ("void radioRefresh()", "void radioSelect("),
            ("void radioSelect(", "void radioSkip("),
            ("void radioToggle()", "void radioPause("),
            ("static bool radioSeekLocked(", "void radioSeek("),
        ]
        functions = "\n".join(source[source.index(a):source.index(b, source.index(a))]
                              for a, b in ranges)
        preamble = r'''
#include "rss_cache.h"
#include <cassert>
#include <cstdint>
#include <string>
using String=std::string;
struct Guard {};
bool initialized=true,enabled=true,want=true,refresh=false,cachePending=true;
bool decoding=true,eof=false,buffering=false;
uint32_t generation=7,feedGeneration=4,baseSec=30,played=32000,fill=90000;
uint32_t head=123,tail=456,audioStart=888,fetchDoneUntil=1234;
int selected=1,bitrate=128,saves=0;
const uint32_t RATE=16000;
String state="playing",error,feedError;
std::vector<radiofeed::Episode> episodes;
void saveResume(){saves++;}
void saveSelection(){}
uint32_t resumePosition(){return 11;}
void clearBuffer(){fill=head=tail=played=0;buffering=true;eof=false;}
'''
        checks = r'''
static radiofeed::Episode episode(const std::string& url){
 radiofeed::Episode e;e.title=url;e.url=url;e.duration=600;return e;
}
int main(){
 episodes={episode("https://example.test/1.mp3"),episode("https://example.test/2.mp3")};
 radioRefresh();assert(refresh&&feedGeneration==5&&generation==7&&want);
 assert(baseSec==30&&played==32000&&fill==90000&&head==123&&tail==456);
 assert(selected==1&&bitrate==128&&audioStart==888&&state=="playing");
 radioRefresh();assert(feedGeneration==5); // coalesce repeated taps
 feedFail(4,"stale");assert(refresh&&feedError.empty());
 feedFail(5,"RSS failed");assert(!refresh&&feedError=="RSS failed"&&want);
 assert(generation==7&&fill==90000&&state=="playing");
 radioRefresh();uint32_t request=feedGeneration;
 radioToggle();assert(!want&&refresh&&state=="paused");
 radioToggle();assert(want&&refresh&&feedGeneration==request);
 assert(radioSeekLocked(50));assert(refresh&&baseSec==50&&want&&feedGeneration==request);
 radioSelect(0,true);assert(refresh&&selected==0&&want&&feedGeneration==request);
 uint32_t playback=generation;uint32_t beforeCancel=feedGeneration;
 uint32_t position=baseSec,pending=fill;int current=selected;
 assert(radioRefreshing());radioCancelRefresh();
 assert(!radioRefreshing()&&feedGeneration==beforeCancel+1&&generation==playback&&want);
 assert(baseSec==position&&fill==pending&&selected==current&&episodes.size()==2);
 feedFail(beforeCancel,"cancelled request failed");assert(feedError.empty());
 radioCancelRefresh();assert(feedGeneration==beforeCancel+1); // idempotent
 radioRefresh();assert(radioRefreshing()&&feedGeneration==beforeCancel+2);
 feedFail(beforeCancel,"stale error");assert(radioRefreshing()&&feedError.empty());
 radioCancelRefresh();episodes.clear();want=false;radioRefresh();assert(state=="loading");
 radioCancelRefresh();assert(state=="idle"&&!refresh&&episodes.empty());
 episodes={episode("https://example.test/1.mp3"),episode("https://example.test/2.mp3")};
 std::vector<radiofeed::Episode> fresh;
 for(int i=0;i<100;i++)fresh.push_back(episode("https://example.test/new"+std::to_string(i)+".mp3"));
 auto merged=radiofeed::mergeCache(fresh,episodes,episodes[0].url);
 assert(merged.size()==102&&radiofeed::episodeIndex(merged,episodes[0].url)>=0);
 auto summary=radiofeed::summarizeFetch({episodes[0],fresh[0],fresh[0]},episodes);
 assert(summary.recent==2&&summary.added==1);
 auto repeat=radiofeed::summarizeFetch(episodes,episodes);
 assert(repeat.recent==2&&repeat.added==0);
 auto first=radiofeed::summarizeFetch(episodes,{});
 assert(first.recent==2&&first.added==2);
 auto reordered=radiofeed::mergeCache({episodes[1],episodes[0]},episodes,episodes[0].url);
 assert(reordered.size()==2&&radiofeed::episodeIndex(reordered,episodes[0].url)==1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "refresh.cpp"
            binary = str(pathlib.Path(tmp) / "refresh-test")
            cpp.write_text(preamble + functions + checks)
            subprocess.run([compiler, "-std=c++11", "-I", str(root / "firmware/src"),
                            str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
