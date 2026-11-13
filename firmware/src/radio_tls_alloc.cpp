// mbedTLS workspace prefers PSRAM; explicit hardware AES/DMA allocations
// remain internal. Keeping only large records external still left too little
// contiguous internal RAM for the SDK AES bounce buffer.
// Linker --wrap targets only the SDK mbedTLS allocator, not general malloc.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdint.h>
#include <stddef.h>
extern "C" void* __real_esp_mbedtls_mem_calloc(size_t n,size_t size);
extern "C" void* __wrap_esp_mbedtls_mem_calloc(size_t n,size_t size){
  if(size && n>SIZE_MAX/size)return nullptr;
  size_t bytes=n*size;
  if(bytes && psramFound()){
    void* out=heap_caps_calloc(n,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(out)return out;
  }
  void* out=__real_esp_mbedtls_mem_calloc(n,size);
  if(!out && bytes && psramFound())
    out=heap_caps_calloc(n,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  return out;
}
