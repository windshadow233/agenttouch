// Visual press feedback only. Does not read touch hardware or dispatch actions.
#pragma once
#include <Arduino_GFX_Library.h>
void buttonFeedbackSet(bool down,int startX,int startY,int x,int y);
bool buttonPressedIn(int x,int y,int w,int h);
void buttonOutline(Arduino_Canvas* c,int x,int y,int w,int h,int radius);
void buttonCircleOutline(Arduino_Canvas* c,int x,int y,int radius);
void buttonPressedOutline(Arduino_Canvas* c,int x,int y,int w,int h,int radius);
