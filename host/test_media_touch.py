"""Check that Mac media controls ignore artwork and blank areas."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class MediaTouchTests(unittest.TestCase):
    def test_mac_buttons_have_bounded_targets(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/pages.cpp").read_text()
        start = source.index("enum { CARD_X")
        constants = source[start:source.index("};", start) + 2]
        start = source.index("int playZoneAt(")
        zone_function = source[start:source.index("\n}", start) + 2]
        function = zone_function + source[source.index("int macPlayZoneAt("): ]
        checks = r'''
int main(){
 assert(macPlayZoneAt(240,GLYPH_CY)==0);
 assert(macPlayZoneAt(80,GLYPH_CY)==-1);
 assert(macPlayZoneAt(400,GLYPH_CY)==1);
 assert(macPlayZoneAt(240,100)==2); // title
 assert(macPlayZoneAt(100,120)==2); // artwork
 assert(macPlayZoneAt(240,SLIDER_Y)==2); // timeline
 assert(macPlayZoneAt(240,450)==2); // footer
 assert(macPlayZoneAt(240,280)==0); // generous center transport band
 assert(macPlayZoneAt(170,GLYPH_CY)==0); // same zone as podcast
 assert(macPlayZoneAt(310,GLYPH_CY)==0);
 assert(macPlayZoneAt(275,GLYPH_CY+35)==0); // modest circular tolerance
 assert(macPlayZoneAt(285,GLYPH_CY+45)==0); // same zone as podcast
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "touch.cpp"
            binary = str(pathlib.Path(tmp) / "touch-test")
            cpp.write_text("#include <cassert>\n#include <cstdlib>\n" + constants + function + checks)
            subprocess.run([compiler, "-std=c++11", str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)

    def test_mac_navigation_uses_standard_swipe_thresholds(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/main.cpp").read_text()
        start = source.index("static int macPageSwipe(")
        swipe = source[start:source.index("\n}", start) + 2]
        start = source.index("static void pageXY(")
        mapping = source[start:source.index("\n}", start) + 2]
        checks = r'''
int main(){
 assert(macPageSwipe(100,100,200,140)==1);
 assert(macPageSwipe(200,140,100,100)==-1);
 assert(macPageSwipe(100,100,180,100)==0);
 assert(macPageSwipe(100,100,181,100)==1);
 assert(macPageSwipe(100,100,200,160)==0);
 assert(macPageSwipe(100,100,140,180)==0);
 assert(macPageSwipe(100,100,108,110)==0);
 // A pinned page can be viewed in every gravity sector. Convert a
 // viewed right/up drag to native coordinates, then use the real mapping.
 for(int rot=0;rot<4;rot++) {
   shownRot=rot;
   int x=240,y=240,rx=x,ry=y,ux=x,uy=y;
   switch((4-rot)&3) {
     case 1: ry+=100;ux+=100;break;
     case 2: rx-=100;uy+=100;break;
     case 3: ry-=100;ux-=100;break;
     default: rx+=100;uy-=100;break;
   }
   int ax,ay,bx,by;
   pageXY(x,y,ax,ay);pageXY(rx,ry,bx,by);
   assert(bx-ax==100 && by==ay);
   assert(macPageSwipe(ax,ay,bx,by)==1); // right: clock -> NP
   assert(macPageSwipe(bx,by,ax,ay)==-1); // left: NP -> clock
   pageXY(ux,uy,bx,by);
   assert(bx==ax && by-ay==-100); // up: volume / reading
 }
 int sx,sy,ex,ey;
 shownRot=3;pageXY(200,100,sx,sy);pageXY(220,200,ex,ey);
 assert(macPageSwipe(sx,sy,ex,ey)==1);
 shownRot=1;pageXY(200,100,sx,sy);pageXY(180,0,ex,ey);
 assert(macPageSwipe(sx,sy,ex,ey)==1);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "swipe.cpp"
            binary = str(pathlib.Path(tmp) / "swipe-test")
            cpp.write_text("#include <cassert>\n#include <cstdlib>\nint shownRot=0;\n#define SWIPE_MIN_PX 80\n" + mapping + swipe + checks)
            subprocess.run([compiler, "-std=c++11", str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
