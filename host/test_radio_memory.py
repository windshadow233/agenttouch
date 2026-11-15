"""Check the ESP32 metadata allocator and bounded parser batch lifetime."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class RadioMemoryTests(unittest.TestCase):
    def test_metadata_uses_psram_and_batches_release(self):
        compiler = shutil.which('c++')
        if not compiler:
            self.skipTest('C++ compiler is not installed')
        root = pathlib.Path(__file__).resolve().parent.parent
        stub = r'''
#pragma once
#include <cstdlib>
#include <cassert>
#include <unordered_map>
const int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
static size_t liveBytes=0,peakBytes=0;
static std::unordered_map<void*,size_t> allocations;
inline void* heap_caps_malloc(size_t size,int caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 void* p=malloc(size);assert(p);allocations[p]=size;liveBytes+=size;
 if(liveBytes>peakBytes)peakBytes=liveBytes;return p;
}
inline void heap_caps_free(void* p){
 auto item=allocations.find(p);assert(item!=allocations.end());
 liveBytes-=item->second;allocations.erase(item);free(p);
}
'''
        source = r'''
#include "rss_cache.h"
#include <cassert>
int main(){
 {
  radiofeed::Parser parser;size_t saved=0,peakBatch=0;
  parser.onBatch=[&](std::vector<radiofeed::Episode>& items){
   if(items.size()>peakBatch)peakBatch=items.size();
   assert(items.size()==16);saved+=items.size();items.clear();return true;
  };
  for(int i=0;i<1500;i++){
   std::string item="<item><title>"+std::string(160,'t')+"</title><enclosure url='https://example.test/"+std::string(100,'u')+std::to_string(i)+"'/></item>";
   parser.write(item.data(),item.size());assert(parser.items.size()<16);
  }
  assert(parser.itemCount==1500&&saved==1488&&parser.items.size()==12&&peakBatch==16);
  auto old=parser.items;
  auto merged=radiofeed::mergeCache(std::move(parser.items),old);
  assert(merged.size()==12&&merged[0].title==std::string(160,'t'));
  assert(peakBytes>0&&peakBytes<20000); // independent of the 1500-item feed
 }
 assert(liveBytes==0&&allocations.empty());
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp)
            (path / 'esp_heap_caps.h').write_text(stub)
            (path / 'memory.cpp').write_text(source)
            binary = str(path / 'memory-test')
            subprocess.run([compiler, '-std=c++11', '-DARDUINO_ARCH_ESP32', '-I', tmp,
                            '-I', str(root / 'firmware/src'), str(path / 'memory.cpp'),
                            '-o', binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == '__main__':
    unittest.main()
