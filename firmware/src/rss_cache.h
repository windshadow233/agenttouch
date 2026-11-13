#pragma once
#include "rss_feed.h"
namespace radiofeed {
constexpr size_t CACHE_LIMIT=100;
inline bool sameCache(const std::vector<Episode>& a,const std::vector<Episode>& b){
 if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(a[i].url!=b[i].url||a[i].title!=b[i].title||a[i].duration!=b[i].duration)return false;return true;
}
inline int episodeIndex(const std::vector<Episode>& items,const std::string& url){
 for(size_t i=0;i<items.size();i++)if(items[i].url==url)return (int)i;return -1;
}
struct FetchSummary {size_t recent=0,added=0;};
inline FetchSummary summarizeFetch(const std::vector<Episode>& fresh,const std::vector<Episode>& old){
 FetchSummary result;std::vector<const std::string*> seen;
 for(const auto& ep:fresh){
  if(!urlOK(ep.url)||result.recent>=CACHE_LIMIT)continue;
  bool duplicate=false;for(const auto* url:seen)if(*url==ep.url)duplicate=true;
  if(duplicate)continue;
  seen.push_back(&ep.url);result.recent++;
  if(episodeIndex(old,ep.url)<0)result.added++;
 }
 return result;
}
inline std::vector<Episode> mergeCache(const std::vector<Episode>& fresh,const std::vector<Episode>& old,const std::string& current=""){
 std::vector<Episode> out;out.reserve(CACHE_LIMIT);
 auto add=[&](const Episode& e){if(!urlOK(e.url)||out.size()>=CACHE_LIMIT)return;for(const auto& v:out)if(v.url==e.url)return;out.push_back(e);};
 for(const auto& e:fresh)add(e);for(const auto& e:old)add(e);
 // Keep the selected episode even when 100 new entries would evict it.
 int previous=episodeIndex(old,current);
 if(previous>=0 && episodeIndex(out,current)<0){
  if(out.size()>=CACHE_LIMIT)out.pop_back();out.push_back(old[previous]);
 }
 return out;
}
}
