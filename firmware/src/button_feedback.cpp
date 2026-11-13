#include "button_feedback.h"
#include "config.h"
#include <stdlib.h>
namespace {
bool held=false;
int startX=0,startY=0,currentX=0,currentY=0;
constexpr uint16_t BLUE=0x249F;
bool inside(int px,int py,int x,int y,int w,int h){return px>=x&&px<x+w&&py>=y&&py<y+h;}
}
void buttonFeedbackSet(bool down,int sx,int sy,int x,int y){
 held=down&&abs(x-sx)<SWIPE_MIN_PX&&abs(y-sy)<VSWIPE_MIN_PX;
 startX=sx;startY=sy;currentX=x;currentY=y;
}
bool buttonPressedIn(int x,int y,int w,int h){
 return held&&inside(startX,startY,x,y,w,h)&&inside(currentX,currentY,x,y,w,h);
}
void buttonOutline(Arduino_Canvas* c,int x,int y,int w,int h,int r){
 c->drawRoundRect(x,y,w,h,r,BLUE);
 c->drawRoundRect(x+1,y+1,w-2,h-2,r>0?r-1:0,BLUE);
}
void buttonCircleOutline(Arduino_Canvas* c,int x,int y,int r){
 c->drawCircle(x,y,r,BLUE);c->drawCircle(x,y,r-1,BLUE);
}
void buttonPressedOutline(Arduino_Canvas* c,int x,int y,int w,int h,int r){
 if(buttonPressedIn(x,y,w,h))buttonOutline(c,x,y,w,h,r);
}
