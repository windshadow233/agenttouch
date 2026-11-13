// Bounded RSS fetch retries. Each attempt starts a fresh parser and connection.
#pragma once
#include "rss_feed.h"
#include "rss_cache.h"
#include <utility>

namespace radiofeed {
template<class MakeReader, class Active, class Pause>
bool fetchWithRetry(Parser& output, std::string& error, MakeReader makeReader,
                    Active active, Pause pause, unsigned attempts=3) {
  for(unsigned attempt=0;attempt<attempts && active();attempt++) {
    Parser parser;
    bool retry=true;
    { // Release the previous HTTP/TLS connection before retrying.
      auto reader=makeReader();
      if(!reader->open()) {
        error=reader->problem.c_str();
        // Permanent HTTP client errors will not improve on a retry.
        if(reader->status>=400 && reader->status<500 &&
           reader->status!=408 && reader->status!=429)retry=false;
      } else {
        uint8_t block[2048];
        bool complete=false;
        while(active() && parser.items.size()<CACHE_LIMIT) {
          int n=reader->read(block,sizeof block);
          if(n<0) {error=reader->problem.c_str();break;}
          if(!n) {complete=true;break;}
          parser.write((const char*)block,(size_t)n);
          if(parser.invalid) {error="RSS is too large or malformed";retry=false;break;}
        }
        if(!active())return false;
        if(!parser.invalid && (complete || parser.items.size()>=CACHE_LIMIT)) {
          if(parser.items.empty()) {error="RSS has no audio enclosure items";retry=false;}
          else {output=std::move(parser);error.clear();return true;}
        }
      }
    }
    if(!active() || !retry || attempt+1>=attempts)return false;
    pause();
  }
  return false;
}
}
