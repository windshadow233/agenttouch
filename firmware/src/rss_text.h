#pragma once
#include <string>
#include <memory>
#include <cstring>
#ifdef ARDUINO_ARCH_ESP32
#include <esp_heap_caps.h>
#endif

namespace radiofeed {
// Episode metadata must not consume the internal RAM needed by SDMMC DMA.
template<class T>struct MetadataAllocator {
 using value_type=T;
 MetadataAllocator()=default;
 template<class U>MetadataAllocator(const MetadataAllocator<U>&){}
 T* allocate(size_t n){
#ifdef ARDUINO_ARCH_ESP32
  if(n>size_t(-1)/sizeof(T))std::__throw_bad_alloc();
  void* p=heap_caps_malloc(n*sizeof(T),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!p)std::__throw_bad_alloc();
  return static_cast<T*>(p);
#else
  return std::allocator<T>{}.allocate(n);
#endif
 }
 void deallocate(T* p,size_t n){
#ifdef ARDUINO_ARCH_ESP32
  (void)n;heap_caps_free(p);
#else
  std::allocator<T>{}.deallocate(p,n);
#endif
 }
 template<class U>bool operator==(const MetadataAllocator<U>&)const{return true;}
 template<class U>bool operator!=(const MetadataAllocator<U>&)const{return false;}
};
class EpisodeText:public std::basic_string<char,std::char_traits<char>,MetadataAllocator<char>> {
 using Base=std::basic_string<char,std::char_traits<char>,MetadataAllocator<char>>;
public:
 using Base::Base;
 using Base::operator=;
 EpisodeText()=default;
 EpisodeText(const std::string& s):Base(s.data(),s.size()){}
 EpisodeText& operator=(const std::string& s){assign(s.data(),s.size());return *this;}
 operator std::string()const{return std::string(data(),size());}
 friend bool operator==(const EpisodeText& a,const EpisodeText& b){return a.compare(b)==0;}
 friend bool operator!=(const EpisodeText& a,const EpisodeText& b){return !(a==b);}
 friend bool operator==(const EpisodeText& a,const char* b){return a.compare(b)==0;}
 friend bool operator!=(const EpisodeText& a,const char* b){return !(a==b);}
 friend bool operator==(const EpisodeText& a,const std::string& b){return a.size()==b.size()&&!memcmp(a.data(),b.data(),a.size());}
 friend bool operator!=(const EpisodeText& a,const std::string& b){return !(a==b);}
};
}
