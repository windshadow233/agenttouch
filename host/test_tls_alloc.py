"""Check that TLS workspace leaves internal AES/DMA memory available."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


class TlsAllocTests(unittest.TestCase):
    def test_small_and_large_workspace_prefer_psram(self):
        compiler = shutil.which("c++")
        if not compiler:
            self.skipTest("C++ compiler is not installed")
        root = pathlib.Path(__file__).resolve().parent.parent
        source = (root / "firmware/src/radio_tls_alloc.cpp").read_text()
        source = "\n".join(line for line in source.splitlines()
                           if not line.startswith("#include"))
        preamble = r'''
#include <cstdint>
#include <cstdlib>
#include <cassert>
const int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
bool present=true,psramFails=false;
int externalCalls=0,internalCalls=0;
bool psramFound(){return present;}
void* heap_caps_calloc(size_t n,size_t s,int caps){
 assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));externalCalls++;
 return psramFails?nullptr:calloc(n,s);
}
extern "C" void* __real_esp_mbedtls_mem_calloc(size_t n,size_t s){internalCalls++;return calloc(n,s);}
'''
        checks = r'''
int main(){
 void* a=__wrap_esp_mbedtls_mem_calloc(1,32);assert(a&&externalCalls==1&&internalCalls==0);free(a);
 a=__wrap_esp_mbedtls_mem_calloc(1,16384);assert(a&&externalCalls==2&&internalCalls==0);free(a);
 present=false;a=__wrap_esp_mbedtls_mem_calloc(1,32);assert(a&&internalCalls==1);free(a);
 present=true;psramFails=true;a=__wrap_esp_mbedtls_mem_calloc(1,32);assert(a&&internalCalls==2);free(a);
 int calls=externalCalls+internalCalls;
 assert(!__wrap_esp_mbedtls_mem_calloc(SIZE_MAX,2));assert(calls==externalCalls+internalCalls);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = pathlib.Path(tmp) / "tls.cpp"
            binary = str(pathlib.Path(tmp) / "tls-test")
            cpp.write_text(preamble + source + checks)
            subprocess.run([compiler, "-std=c++11", str(cpp), "-o", binary], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    unittest.main()
