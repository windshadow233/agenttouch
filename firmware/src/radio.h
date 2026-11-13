// Standalone Wi-Fi RSS/MP3 player. Network and decoding never run on loopTask.
#pragma once
#include <ArduinoJson.h>
#include "player.h"
bool radioInit();
bool radioEnabled();
bool radioConfigure(const char* feed, bool enabled);
void radioRefresh();
void radioDisable();
void radioPoll();
void radioToggle();
void radioPause();
void radioSkip(int delta);
void radioSelect(int index, bool play=true);
void radioSeek(int32_t delta);
bool radioSeekTo(uint32_t seconds);
bool radioPlaying();
bool radioWantPlay();
float radioBufferedSeconds(); // decoded audio ready to play, excluding network estimates
uint32_t radioPull(int16_t* out, uint32_t maxSamples);
int radioCount();
bool radioEpisode(int index,char* title,size_t size,unsigned* duration=nullptr);
const PlayerInfo& radioInfo();
void radioStatus(JsonObject out);
