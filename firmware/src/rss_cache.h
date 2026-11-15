#pragma once
#include "rss_feed.h"
namespace radiofeed {
inline bool sameCache(const std::vector<Episode>& a,const std::vector<Episode>& b){
 if(a.size()!=b.size())return false;for(size_t i=0;i<a.size();++i)if(a[i].url!=b[i].url||a[i].title!=b[i].title||a[i].duration!=b[i].duration)return false;return true;
}
inline int episodeIndex(const std::vector<Episode>& items,const std::string& url){
 for(size_t i=0;i<items.size();i++)if(items[i].url==url)return (int)i;return -1;
}
struct FetchSummary {size_t recent=0,added=0;};
inline FetchSummary summarizeFetch(const std::vector<Episode>& fresh,const std::vector<Episode>& old){
 FetchSummary result;std::vector<const EpisodeText*> seen;
 for(const auto& ep:fresh){
  if(!urlOK(ep.url))continue;
  bool duplicate=false;for(const auto* url:seen)if(*url==ep.url)duplicate=true;
  if(duplicate)continue;
  seen.push_back(&ep.url);result.recent++;
  if(episodeIndex(old,ep.url)<0)result.added++;
 }
 return result;
}
inline std::vector<Episode> mergeCache(std::vector<Episode> fresh,const std::vector<Episode>& old,const std::string& current=""){
 fresh.reserve(fresh.size()+old.size());
 for(size_t i=0;i<fresh.size();){
  bool duplicate=!urlOK(fresh[i].url);
  for(size_t j=0;j<i&&!duplicate;j++)duplicate=fresh[j].url==fresh[i].url;
  if(duplicate)fresh.erase(fresh.begin()+i);else i++;
 }
 for(const auto& ep:old)if(urlOK(ep.url)&&episodeIndex(fresh,ep.url)<0)fresh.push_back(ep);
 (void)current; // All old entries remain, including the selected episode.
 return fresh;
}
}
