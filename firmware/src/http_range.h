// Strict Content-Range validation before resuming an HTTP body.
#pragma once
#include <cstdint>
#include <string>
namespace radiohttp {
struct Range {uint64_t first=0,last=0,size=0;};
inline bool number(const std::string& s,size_t& i,uint64_t& value){
 value=0;size_t begin=i;
 while(i<s.size()&&s[i]>='0'&&s[i]<='9'){
  unsigned digit=s[i++]-'0';if(value>(UINT64_MAX-digit)/10)return false;
  value=value*10+digit;
 }
 return i>begin;
}
inline bool parseRange(const std::string& s,Range& r){
 if(s.compare(0,6,"bytes "))return false;size_t i=6;
 if(!number(s,i,r.first)||i>=s.size()||s[i++]!='-')return false;
 if(!number(s,i,r.last)||i>=s.size()||s[i++]!='/')return false;
 return number(s,i,r.size)&&i==s.size()&&r.first<=r.last&&r.last<r.size;
}
inline bool validRange(const Range& r,uint64_t offset,uint64_t expectedSize,
                       int length,uint32_t limit){
 return r.first==offset&&(!expectedSize||r.size==expectedSize)&&
   (length<0||(uint64_t)length==r.last-r.first+1)&&
   (!limit||r.last-r.first+1<=limit);
}
}
