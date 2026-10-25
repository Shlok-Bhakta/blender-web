// SPDX-License-Identifier: GPL-2.0-or-later
#include <OpenImageDenoise/oidn.h>
#define IMATH_HALF_NO_LOOKUP_TABLE
#include <Imath/half.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include <chrono>
static void check(OIDNDevice dev) {const char* message=nullptr;auto error=oidnGetDeviceError(dev,&message);if(error!=OIDN_ERROR_NONE){fprintf(stderr,"OIDN %d: %s\n",int(error),message);std::abort();}}
int main(int argc,char**argv){
 auto device=oidnNewDevice(OIDN_DEVICE_TYPE_CPU);assert(device);oidnSetDeviceInt(device,"numThreads",4);oidnSetDeviceBool(device,"setAffinity",false);oidnCommitDevice(device);check(device);
 constexpr size_t W=61,H=45,N=W*H*3;
 std::vector<float> color(N),normal(N),albedo(N),clean(N),out(N);
 uint32_t rng=2348529;
 for(size_t y=0;y<H;y++)for(size_t x=0;x<W;x++)for(size_t c=0;c<3;c++){
  size_t i=(y*W+x)*3+c;float v=.25f+.3f*float(x)/W+.1f*c+.12f*(x>W/2);clean[i]=v;
  rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;color[i]=v+.22f*(float(rng&65535)/65535-.5f);
  albedo[i]=.5f;normal[i]=(c==2)?1:0;
 }
 printf("{\"version\":%d,\"cases\":[",oidnGetDeviceInt(device,"version"));
 for(int guided=0;guided<2;guided++){
  auto filter=oidnNewFilter(device,"RT");assert(filter);
  oidnSetSharedFilterImage(filter,"color",color.data(),OIDN_FORMAT_FLOAT3,W,H,0,0,0);
  oidnSetSharedFilterImage(filter,"output",out.data(),OIDN_FORMAT_FLOAT3,W,H,0,0,0);
  if(guided){oidnSetSharedFilterImage(filter,"albedo",albedo.data(),OIDN_FORMAT_FLOAT3,W,H,0,0,0);oidnSetSharedFilterImage(filter,"normal",normal.data(),OIDN_FORMAT_FLOAT3,W,H,0,0,0);oidnSetFilterBool(filter,"cleanAux",true);}
  oidnSetFilterBool(filter,"hdr",true);oidnCommitFilter(filter);check(device);
  auto start=std::chrono::steady_clock::now();oidnExecuteFilter(filter);check(device);
  double sec=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(),before=0,after=0,sum=0;
  for(size_t i=0;i<N;i++){assert(std::isfinite(out[i]));before+=std::pow(color[i]-clean[i],2);after+=std::pow(out[i]-clean[i],2);sum+=out[i];}
  assert(after<before*.75);if(guided)printf(",");printf("{\"guided\":%d,\"seconds\":%.8f,\"inputMSE\":%.9f,\"outputMSE\":%.9f,\"sum\":%.8f}",guided,sec,before/N,after/N,sum);
  if(argc>1){char path[1024];snprintf(path,sizeof(path),"%s-%d.f32",argv[1],guided);auto f=fopen(path,"wb");assert(f);assert(fwrite(out.data(),sizeof(float),N,f)==N);fclose(f);}
  oidnReleaseFilter(filter);
 }
 // Half images exercise the public conversion API through the FP32 network.
 std::vector<uint16_t> halfColor(N),halfOut(N);
 for(size_t i=0;i<N;i++)halfColor[i]=imath_float_to_half(color[i]);
 auto filter=oidnNewFilter(device,"RT");oidnSetSharedFilterImage(filter,"color",halfColor.data(),OIDN_FORMAT_HALF3,W,H,0,0,0);oidnSetSharedFilterImage(filter,"output",halfOut.data(),OIDN_FORMAT_HALF3,W,H,0,0,0);oidnSetFilterBool(filter,"hdr",true);oidnCommitFilter(filter);check(device);oidnExecuteFilter(filter);check(device);
 double halfMSE=0;for(size_t i=0;i<N;i++){float v=imath_half_to_float(halfOut[i]);assert(std::isfinite(v));halfMSE+=std::pow(v-clean[i],2);}assert(halfMSE/N<.001);
 oidnSetFilterProgressMonitorFunction(filter,[](void*,double){return false;},nullptr);
 oidnExecuteFilter(filter);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_CANCELLED);
 oidnSetFilterProgressMonitorFunction(filter,nullptr,nullptr);oidnExecuteFilter(filter);check(device);
 oidnReleaseFilter(filter);
 auto buffer=oidnNewBuffer(device,64);check(device);filter=oidnNewFilter(device,"RT");
 oidnSetFilterImage(filter,"color",buffer,OIDN_FORMAT_FLOAT3,65536,65536,0,0,0);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
 oidnSetFilterImage(filter,"color",buffer,OIDN_FORMAT_FLOAT3,2,2,60,0,0);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
#ifdef __EMSCRIPTEN__
 oidnSetFilterImage(filter,"color",buffer,OIDN_FORMAT_FLOAT3,2,2,0,0x80000000u,0);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
 oidnSetFilterImage(filter,"color",buffer,OIDN_FORMAT_FLOAT3,2,2,0,12,0x80000000u);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
 oidnSetFilterImage(filter,"color",buffer,OIDN_FORMAT_FLOAT3,1,1,0xfffffffcu,0,0);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
#endif
 oidnReleaseFilter(filter);
#ifdef __EMSCRIPTEN__
 uint8_t bytes[16]={};
 oidnReadBuffer(buffer,1,std::numeric_limits<size_t>::max(),bytes);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
 oidnWriteBuffer(buffer,1,std::numeric_limits<size_t>::max(),bytes);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_ARGUMENT);
 oidnWriteBuffer(buffer,0,sizeof(bytes),bytes);oidnReadBuffer(buffer,0,sizeof(bytes),bytes);check(device);
 // A 64-bit TZA offset must not wrap into an apparently valid Wasm32 address.
 std::vector<uint8_t> weights(64);weights[0]=0xd7;weights[1]=0x41;weights[2]=2;
 uint64_t offset=0x10000000cull;std::memcpy(weights.data()+4,&offset,8);
 filter=oidnNewFilter(device,"RT");oidnSetSharedFilterImage(filter,"color",color.data(),OIDN_FORMAT_FLOAT3,W,H,0,0,0);oidnSetSharedFilterImage(filter,"output",out.data(),OIDN_FORMAT_FLOAT3,W,H,0,0,0);
 oidnSetSharedFilterData(filter,"weights",weights.data(),weights.size());oidnCommitFilter(filter);assert(oidnGetDeviceError(device,nullptr)==OIDN_ERROR_INVALID_OPERATION);oidnReleaseFilter(filter);
#endif
 oidnReleaseBuffer(buffer);oidnReleaseDevice(device);
 printf("],\"halfMSE\":%.9f,\"invalidSizes\":true,\"cancellationRecovery\":true}\n",halfMSE/N);
}
