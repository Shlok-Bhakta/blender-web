// SPDX-License-Identifier: GPL-2.0-or-later
#include "ispc-half.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
extern "C" int mask_eq4(uint32_t,uint32_t);
extern "C" int mask_ne4(uint32_t,uint32_t);
extern "C" int mask_eq8(uint32_t,uint32_t);
extern "C" int mask_ne8(uint32_t,uint32_t);
extern "C" int mask_eq16(uint32_t,uint32_t);
extern "C" int mask_ne16(uint32_t,uint32_t);
static float decode(uint16_t h) {
  unsigned exponent=(h>>10)&31, fraction=h&1023;
  float value=exponent==31?(fraction?std::numeric_limits<float>::quiet_NaN():INFINITY):
    std::ldexp(float(exponent?1024+fraction:fraction),exponent?int(exponent)-25:-24);
  return std::copysign(value,h&0x8000?-1.f:1.f);
}
static uint16_t encode(float value) {
  unsigned sign=std::signbit(value)?0x8000:0;double a=std::abs(double(value));
  if(std::isnan(a))return sign|0x7e00;
  if(a>=65520)return sign|0x7c00;
  if(a<0x1p-14)return sign|unsigned(std::nearbyint(std::ldexp(a,24)));
  int exponent;std::frexp(a,&exponent);
  unsigned significand=unsigned(std::nearbyint(std::ldexp(a,11-exponent)));
  if(significand==2048){significand=1024;++exponent;}
  return sign|((exponent+14)<<10)|(significand-1024);
}
int main(){
  std::vector<float> input;
  for(unsigned h=0;h<65536;h++)input.push_back(decode(h));
  for(unsigned h=0;h<0x7bff;h++) {
    float midpoint=(decode(h)+decode(h+1))*.5f;
    for(float sign:{1.f,-1.f})for(float value:{std::nextafter(midpoint,-INFINITY),midpoint,std::nextafter(midpoint,INFINITY)})input.push_back(sign*value);
  }
  // Also cover FP32 underflow, overflow and arbitrary exponent/payload bits.
  uint32_t rng=34781;
  for(unsigned i=0;i<65537;i++){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;float f;std::memcpy(&f,&rng,4);input.push_back(f);}
  std::vector<uint16_t> half(input.size());std::vector<float> output(input.size());
  ispc::half_roundtrip(input.data(),reinterpret_cast<__fp16*>(half.data()),output.data(),int(input.size()));
  for(size_t i=0;i<input.size();i++){
    uint16_t expected=encode(input[i]);
    if(std::isnan(input[i])){assert((half[i]&0x7c00)==0x7c00&&(half[i]&1023));assert(std::isnan(output[i]));}
    else {assert(half[i]==expected);assert(output[i]==decode(expected));assert(std::signbit(output[i])==std::signbit(decode(expected)));}
  }
  std::vector<uint16_t> a(65537),b(a.size());std::vector<int> greater(a.size());
  for(size_t i=0;i<a.size();i++){a[i]=uint16_t(i);b[i]=uint16_t(i*173+32731);}
  ispc::half_greater(reinterpret_cast<__fp16*>(a.data()),reinterpret_cast<__fp16*>(b.data()),greater.data(),int(a.size()));
  for(size_t i=0;i<a.size();i++)assert(greater[i]==(decode(a[i])>decode(b[i])));
  for(uint32_t x=0;x<256;x++)for(uint32_t y=0;y<256;y++) {
    assert(mask_eq4(x,y)==((x&15)==(y&15)));assert(mask_ne4(x,y)==((x&15)!=(y&15)));
    assert(mask_eq8(x,y)==(x==y));assert(mask_ne8(x,y)==(x!=y));
  }
  for(uint32_t x=0;x<65536;x++)for(uint32_t y:{x,x^1,x^32768,x^65535}){
    assert(mask_eq16(x,y)==(x==y));assert(mask_ne16(x,y)==(x!=y));
  }
  printf("{\"halfConversions\":%zu,\"halfComparisons\":%zu,\"packedMaskComparisons\":786432}\n",input.size(),a.size());
}
