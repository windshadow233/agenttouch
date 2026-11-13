#pragma once
#include <cstdint>
#include <cstddef>
// Only block for more data when the decoder actually needs it.
template<class Available>
bool radioNeedsInput(size_t used,bool done,bool needsInput,size_t frameReserve,Available available){
 return !done&&(needsInput||used<frameReserve||(used<16384&&available()));
}
// Stateful linear resampler; output goes directly into a bounded PCM ring.
struct RadioResampler {
  int rate=0,phase=0;int16_t previous=0;bool primed=false;
  template<class Emit> bool push(const int16_t* pcm,int frames,int channels,int hz,Emit emit){
    if(channels<1||channels>2||hz<8000||hz>48000)return false;
    if(rate!=hz){rate=hz;phase=0;primed=false;}
    for(int i=0;i<frames;++i){
      int sample=pcm[i*channels];if(channels==2)sample=(sample+pcm[i*2+1])/2;
      if(!primed){previous=(int16_t)sample;primed=true;if(!emit(previous))return false;continue;}
      phase+=16000;
      while(phase>=rate){phase-=rate;int out=sample+((int64_t)(previous-sample)*phase)/16000;if(!emit((int16_t)out))return false;}
      previous=(int16_t)sample;
    }return true;
  }
};
