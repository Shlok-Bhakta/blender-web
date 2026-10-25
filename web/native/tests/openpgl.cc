// SPDX-License-Identifier: GPL-2.0-or-later
#include <openpgl/cpp/OpenPGL.h>
#include <embree4/rtcore.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
namespace pgl = openpgl::cpp;
int main(){
 auto rtc=rtcNewDevice("threads=4");assert(rtc);rtcReleaseDevice(rtc);
 pgl::Device device(PGL_DEVICE_TYPE_CPU_4,4);
 std::printf("{\"models\":[");int modelIndex=0;
 for(auto kind:{PGL_DIRECTIONAL_DISTRIBUTION_PARALLAX_AWARE_VMM,PGL_DIRECTIONAL_DISTRIBUTION_QUADTREE,PGL_DIRECTIONAL_DISTRIBUTION_VMM}){
  pgl::FieldConfig config;config.Init(PGL_SPATIAL_STRUCTURE_KDTREE,kind,true,16384);pgl::Field field(&device,config);
  field.SetSceneBounds({{-1,-1,-1},{1,1,1}});
  pgl::SampleStorage storage;
  for(unsigned i=0;i<4096;i++){
   pgl::SampleData value{};value.position={float(i%16)/16-.5f,float((i/16)%16)/16-.5f,float(i/256)/16-.5f};
   float x=.18f*std::sin(i*2.39996323f),y=.18f*std::cos(i*2.39996323f);value.direction={x,y,std::sqrt(1-x*x-y*y)};
   value.weight=1;value.pdf=1;value.distance=10;storage.AddSample(value);value.flags=PGLSampleData::EInsideVolume;storage.AddSample(value);
  }
  assert(storage.Validate()&&storage.GetSizeSurface()==4096&&storage.GetSizeVolume()==4096);
  field.Update(storage);field.Update(storage);assert(field.Validate()&&field.GetIteration()==2);
  pgl::SurfaceSamplingDistribution surface(&field);pgl::VolumeSamplingDistribution volume(&field);float u=.3f;
  assert(surface.Init(&field,{0,0,0},u));u=.3f;assert(volume.Init(&field,{0,0,0},u));
  assert(surface.Validate()&&volume.Validate());float front=surface.PDF({0,0,1}),back=surface.PDF({0,0,-1});assert(front>back&&front>0);
  float mean=0;for(unsigned i=0;i<1024;i++){pgl_vec3f dir;float pdf=surface.SamplePDF({(i%32+.5f)/32,(i/32+.5f)/32},dir);assert(std::isfinite(pdf)&&pdf>0);float check=surface.PDF(dir);assert(std::abs(pdf-check)<1e-3f*std::max(1.f,pdf));mean+=dir.z;}
  mean/=1024;assert(mean>.7f);
  if(surface.SupportsApplyCosineProduct()){surface.ApplyCosineProduct({0,0,1});assert(surface.Validate());}
  if(volume.SupportsApplySingleLobeHenyeyGreensteinProduct()){volume.ApplySingleLobeHenyeyGreensteinProduct({0,0,1},.5f);assert(volume.Validate());}
  assert(field.Store("/tmp/openpgl-check.bin"));pgl::Field restored(&device,std::string("/tmp/openpgl-check.bin"));assert(restored.Validate()&&restored.GetIteration()==2);
  if(modelIndex++)std::printf(",");std::printf("{\"kind\":%d,\"frontPdf\":%.8f,\"backPdf\":%.8f,\"meanZ\":%.8f}",int(kind),front,back,mean);
 }
 std::printf("]}\n");
}
