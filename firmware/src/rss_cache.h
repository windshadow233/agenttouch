#pragma once
#include "rss_feed.h"
namespace radiofeed {
constexpr size_t CACHE_LIMIT=100;
inline bool sameCache(const std::vector<Episode>& a,const std::vector<Episode>& b){
 if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(a[i].url!=b[i].url||a[i].title!=b[i].title||a[i].duration!=b[i].duration)return false;return true;
}
inline std::vector<Episode> mergeCache(const std::vector<Episode>& fresh,const std::vector<Episode>& old){
 std::vector<Episode> out;out.reserve(CACHE_LIMIT);
 auto add=[&](const Episode& e){if(!urlOK(e.url)||out.size()>=CACHE_LIMIT)return;for(const auto& v:out)if(v.url==e.url)return;out.push_back(e);};
 for(const auto& e:fresh)add(e);for(const auto& e:old)add(e);return out;
}
}
