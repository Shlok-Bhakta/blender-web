/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <embree4/rtcore.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

struct Vertex { float x, y, z, pad; };
struct Triangle { unsigned a, b, c; };
static RTCRayHit ray(float x, float y, float time=0, unsigned mask=~0u)
{
  RTCRayHit hit{};
  hit.ray.org_x=x; hit.ray.org_y=y; hit.ray.org_z=5;
  hit.ray.dir_z=-1; hit.ray.tfar=100; hit.ray.time=time; hit.ray.mask=mask;
  hit.hit.geomID=RTC_INVALID_GEOMETRY_ID;
  for (auto &id:hit.hit.instID) id=RTC_INVALID_GEOMETRY_ID;
  return hit;
}
static unsigned plane(RTCDevice device, RTCScene scene, float z, unsigned mask=~0u, bool motion=false)
{
  RTCGeometry geometry=rtcNewGeometry(device,RTC_GEOMETRY_TYPE_TRIANGLE);
  assert(geometry);
  if(motion) rtcSetGeometryTimeStepCount(geometry,2);
  for(unsigned step=0;step<(motion?2u:1u);step++) {
    auto *v=static_cast<Vertex*>(rtcSetNewGeometryBuffer(geometry,RTC_BUFFER_TYPE_VERTEX,step,
        RTC_FORMAT_FLOAT3,sizeof(Vertex),4));
    assert(v);
    v[0]={0,0,z+step,0};v[1]={32,0,z+step,0};v[2]={0,32,z+step,0};v[3]={32,32,z+step,0};
  }
  auto *t=static_cast<Triangle*>(rtcSetNewGeometryBuffer(geometry,RTC_BUFFER_TYPE_INDEX,0,
      RTC_FORMAT_UINT3,sizeof(Triangle),2));
  assert(t); t[0]={0,1,2};t[1]={1,3,2};
  rtcSetGeometryMask(geometry,mask);rtcCommitGeometry(geometry);
  unsigned id=rtcAttachGeometry(scene,geometry);rtcReleaseGeometry(geometry);
  return id;
}
static std::atomic<unsigned> rejected{0};
static void reject(const RTCFilterFunctionNArguments *args)
{
  for(unsigned i=0;i<args->N;i++) if(args->valid[i]) {args->valid[i]=0;rejected++;}
}
int main()
{
  RTCDevice device=rtcNewDevice("threads=8");assert(device);
  assert(rtcGetDeviceProperty(device,RTC_DEVICE_PROPERTY_VERSION)==40401);
  RTCScene scene=rtcNewScene(device);assert(scene);
  unsigned id=plane(device,scene,0,2);rtcCommitScene(scene);
  auto hit=ray(3,7,0,2);rtcIntersect1(scene,&hit);
  assert(hit.hit.geomID==id && std::abs(hit.ray.tfar-5)<1e-5 && hit.hit.Ng_z>0);
  auto miss=ray(33,7,0,2);rtcIntersect1(scene,&miss);assert(miss.hit.geomID==RTC_INVALID_GEOMETRY_ID);
  auto masked=ray(3,7,0,1);rtcIntersect1(scene,&masked);assert(masked.hit.geomID==RTC_INVALID_GEOMETRY_ID);
  auto shadow=ray(3,7,0,2).ray;rtcOccluded1(scene,&shadow);assert(std::isinf(shadow.tfar)&&shadow.tfar<0);
  alignas(16) RTCRayHit4 packet{};alignas(16) int valid[4]={-1,-1,-1,0};
  for(int i=0;i<4;i++) {
    packet.ray.org_x[i]=i+1;packet.ray.org_y[i]=4;packet.ray.org_z[i]=5;
    packet.ray.dir_z[i]=-1;packet.ray.tfar[i]=100;packet.ray.mask[i]=2;
    packet.hit.geomID[i]=RTC_INVALID_GEOMETRY_ID;
  }
  rtcIntersect4(valid,scene,&packet);
  for(int i=0;i<3;i++) assert(packet.hit.geomID[i]==id && std::abs(packet.ray.tfar[i]-5)<1e-4);
  assert(packet.hit.geomID[3]==RTC_INVALID_GEOMETRY_ID);
  std::mutex mutex;std::set<std::thread::id> workers;std::atomic<unsigned> hits{0};
  auto started=std::chrono::steady_clock::now();
  tbb::task_arena arena(8);
  arena.execute([&] {tbb::parallel_for(0u,65536u,[&](unsigned i) {
    auto r=ray((i%31)+.25f,((i/31)%31)+.25f,0,2);rtcIntersect1(scene,&r);
    assert(r.hit.geomID==id && std::abs(r.ray.tfar-5)<1e-4);hits++;
    if(i%256==0) {std::lock_guard<std::mutex> lock(mutex);workers.insert(std::this_thread::get_id());}
  });});
  assert(hits==65536);
  double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  RTCScene instanced=rtcNewScene(device);RTCGeometry instance=rtcNewGeometry(device,RTC_GEOMETRY_TYPE_INSTANCE);
  rtcSetGeometryInstancedScene(instance,scene);
  rtcSetGeometryMask(instance,~0u);
  const float transform[12]={1,0,0,100,0,1,0,0,0,0,1,0};
  rtcSetGeometryTransform(instance,0,RTC_FORMAT_FLOAT3X4_ROW_MAJOR,transform);
  rtcCommitGeometry(instance);unsigned instance_id=rtcAttachGeometry(instanced,instance);
  rtcReleaseGeometry(instance);rtcCommitScene(instanced);
  auto transformed=ray(103,7,0,2);rtcIntersect1(instanced,&transformed);
  assert(transformed.hit.geomID==id && transformed.hit.instID[0]==instance_id && std::abs(transformed.ray.tfar-5)<1e-4);
  RTCScene animated=rtcNewScene(device);unsigned moving=plane(device,animated,0,~0u,true);rtcCommitScene(animated);
  auto interpolated=ray(3,7,.5f);rtcIntersect1(animated,&interpolated);
  assert(interpolated.hit.geomID==moving && std::abs(interpolated.ray.tfar-4.5f)<1e-5);
  RTCScene filtered=rtcNewScene(device);unsigned front=plane(device,filtered,0);unsigned back=plane(device,filtered,-1);
  RTCGeometry front_geometry=rtcGetGeometry(filtered,front);rtcSetGeometryIntersectFilterFunction(front_geometry,reject);
  rtcCommitGeometry(front_geometry);rtcCommitScene(filtered);
  auto through=ray(3,7);rtcIntersect1(filtered,&through);
  assert(rejected>0 && through.hit.geomID==back && std::abs(through.ray.tfar-6)<1e-4);
  RTCScene primitives=rtcNewScene(device);
  RTCGeometry sphere=rtcNewGeometry(device,RTC_GEOMETRY_TYPE_SPHERE_POINT);
  auto *center=static_cast<Vertex*>(rtcSetNewGeometryBuffer(sphere,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT4,sizeof(Vertex),1));
  assert(center);center[0]={1,1,0,.5f};rtcCommitGeometry(sphere);
  unsigned sphere_id=rtcAttachGeometry(primitives,sphere);rtcReleaseGeometry(sphere);
  RTCGeometry curve=rtcNewGeometry(device,RTC_GEOMETRY_TYPE_ROUND_LINEAR_CURVE);
  auto *control=static_cast<Vertex*>(rtcSetNewGeometryBuffer(curve,RTC_BUFFER_TYPE_VERTEX,0,RTC_FORMAT_FLOAT4,sizeof(Vertex),2));
  assert(control);control[0]={4,0,0,.25f};control[1]={4,4,0,.25f};
  auto *segment=static_cast<unsigned*>(rtcSetNewGeometryBuffer(curve,RTC_BUFFER_TYPE_INDEX,0,RTC_FORMAT_UINT,sizeof(unsigned),1));
  assert(segment);segment[0]=0;rtcCommitGeometry(curve);
  unsigned curve_id=rtcAttachGeometry(primitives,curve);rtcReleaseGeometry(curve);rtcCommitScene(primitives);
  auto point_ray=ray(1,1);rtcIntersect1(primitives,&point_ray);
  assert(point_ray.hit.geomID==sphere_id && std::abs(point_ray.ray.tfar-4.5f)<1e-4);
  auto curve_ray=ray(4,2);rtcIntersect1(primitives,&curve_ray);
  assert(curve_ray.hit.geomID==curve_id && std::abs(curve_ray.ray.tfar-4.75f)<1e-4);
  rtcReleaseScene(primitives);
  assert(rtcGetDeviceError(device)==RTC_ERROR_NONE);
  RTCGeometry invalid=rtcNewGeometry(device,static_cast<RTCGeometryType>(999));
  assert(!invalid && rtcGetDeviceError(device)!=RTC_ERROR_NONE);
  auto recovered=ray(3,7,0,2);rtcIntersect1(scene,&recovered);assert(recovered.hit.geomID==id);
  assert(rtcGetDeviceError(device)==RTC_ERROR_NONE);
  rtcReleaseScene(filtered);rtcReleaseScene(animated);rtcReleaseScene(instanced);rtcReleaseScene(scene);rtcReleaseDevice(device);
  std::printf("{\"version\":\"4.4.1\",\"parallelRays\":65536,\"workers\":%zu,\"seconds\":%.8f,\"rayPacket4\":true,\"maskedMiss\":true,\"occlusion\":true,\"instance\":true,\"motion\":true,\"spherePoint\":true,\"roundCurve\":true,\"filter\":true,\"invalidGeometryRecovery\":true}\n",workers.size(),seconds);
}
