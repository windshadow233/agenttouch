// Bounded RSS retries retain the parser and reader after body bytes arrive.
#pragma once
#include "rss_feed.h"
#include "rss_cache.h"
#include <utility>
#include <algorithm>
#include <cerrno>
#include <cstdio>

namespace radiofeed {
struct IgnoreFetchProgress {void operator()(size_t,size_t)const{}};
template<class MakeReader, class Active, class Pause, class Progress=IgnoreFetchProgress>
bool fetchWithRetry(Parser& output, std::string& error, MakeReader makeReader,
                    Active active, Pause pause, unsigned attempts=3, Progress progress=Progress{}) {
  Parser parser;
  progress(0,0);
  auto reader=makeReader();
  for(unsigned attempt=0;attempt<attempts && active();attempt++) {
    // resume() preserves the exact delivered byte offset and entity validator.
    // Never append a replacement body or silently discard downloaded progress.
    bool opened=parser.bytes?reader->resume():reader->open();
    if(!opened) {
      error=reader->problem.c_str();
      if(!reader->retryable())return false;
    } else {
      uint8_t block[2048];
      bool complete=false;
      while(active()) {
        int n=reader->read(block,sizeof block);
        if(n<0) {error=reader->problem.c_str();break;}
        if(!n) {complete=true;break;}
        parser.write((const char*)block,(size_t)n);
        progress(parser.bytes,parser.items.size());
        if(parser.invalid) {error="RSS is too large or malformed";return false;}
      }
      if(!active())return false;
      if(complete) {
        if(parser.items.empty()) {error="RSS has no audio enclosure items";return false;}
        output=std::move(parser);error.clear();return true;
      }
      if(!reader->retryable())return false;
    }
    if(!active() || attempt+1>=attempts)return false;
    pause();
  }
  return false;
}
}

namespace radiofeed {
// Download to an SD-backed file first, then parse it without network waits.
// The same reader and partial file survive every automatic resume attempt.
struct SeekSavedFile {
 template<class File>bool operator()(File& file,size_t offset)const{return file.seek(offset);}
};
struct KeepEpisodeBatch {bool operator()(std::vector<Episode>&,bool)const{return true;}};
struct ReadSavedFile {
 template<class File>int operator()(File& file,uint8_t* block,size_t size)const{return file.read(block,size);}
};
template<class File, class MakeReader, class Active, class Pause, class Progress, class PrepareRead=SeekSavedFile, class ReadFile=ReadSavedFile, class SaveBatch=KeepEpisodeBatch>
bool fetchFileWithRetry(Parser& output,std::string& error,File& file,
                        MakeReader makeReader,Active active,Pause pause,
                        Progress progress,unsigned attempts=3,PrepareRead prepareRead=PrepareRead{},
                        uint8_t* transferBuffer=nullptr,size_t transferSize=0,ReadFile readFile=ReadFile{},SaveBatch saveBatch=SaveBatch{}) {
 auto reader=makeReader();size_t downloaded=0,filled=0;bool complete=false;
 uint8_t fallback[2048];
 uint8_t* block=transferBuffer?transferBuffer:fallback;
 size_t capacity=transferBuffer?transferSize:sizeof fallback;
 if(!capacity){error="RSS transfer buffer unavailable";return false;}
 auto flushBlock=[&](){
  if(filled&&file.write(block,filled)!=filled){error="SD RSS file write failed";return false;}
  filled=0;return true;
 };
 progress(0,0,false);
 for(unsigned attempt=0;attempt<attempts&&active();attempt++){
  bool opened=downloaded?reader->resume():reader->open();
  if(opened){
   while(active()){
    int n=reader->read(block+filled,capacity-filled);
    if(n<0){error=reader->problem.c_str();break;}
    if(!n){complete=true;break;}
    if(downloaded+(size_t)n>4*1024*1024){error="RSS file exceeds 4 MB";return false;}
    filled+=n;downloaded+=n;
    if(filled==capacity&&!flushBlock())return false;
    progress(downloaded,0,false);
   }
  }else error=reader->problem.c_str();
  if(complete)break;
  if(!active()||!reader->retryable()||attempt+1>=attempts)return false;
  if(!flushBlock())return false;
  file.flush();pause();
 }
 if(!active()||!complete)return false;
 reader.reset(); // Release TLS memory before parsing the saved file.
 if(!flushBlock())return false;
 file.flush();
 if(!prepareRead(file,0)){error="SD RSS file reopen/size check failed";return false;}
 Parser parser;bool batchFailed=false;
 parser.onBatch=[&](std::vector<Episode>& batch){
  if(!saveBatch(batch,false)){batchFailed=true;return false;}
  return true;
 };
 progress(downloaded,0,true);
 while(active()&&parser.bytes<downloaded){
  size_t wanted=std::min(capacity,downloaded-parser.bytes);
  int n=0,readError=0;
  for(unsigned retry=0;retry<3&&active();retry++){
   errno=0;n=readFile(file,block,wanted);readError=errno;
   if(n>0)break;
   // Reopen clears a latched stdio EOF/error without re-downloading RSS.
   if(retry<2&&!prepareRead(file,parser.bytes))break;
  }
  if(n<=0){
   char detail[144];snprintf(detail,sizeof detail,"SD RSS file read failed at %lu/%lu, wanted %lu, errno %d",
     (unsigned long)parser.bytes,(unsigned long)downloaded,(unsigned long)wanted,readError);
   error=detail;return false;
  }
  parser.write((const char*)block,(size_t)n);
  progress(downloaded,parser.itemCount,true);
  if(parser.invalid){error=batchFailed?"SD RSS metadata batch write failed":"RSS is too large or malformed";return false;}
 }
 if(!active())return false;
 if(!parser.itemCount){error="RSS has no audio enclosure items";return false;}
 if(!saveBatch(parser.items,true)){error="SD RSS metadata batch write failed";return false;}
 parser.onBatch={}; // Do not retain callbacks that capture the local writer.
 output=std::move(parser);error.clear();return true;
}
}
