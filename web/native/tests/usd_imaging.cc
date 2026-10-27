/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/hio/fieldTextureData.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/plug/plugin.h>
#include <pxr/base/tf/type.h>
#include <OpenImageIO/imageio.h>
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/base/tf/error.h>
#include <pxr/base/tf/warning.h>
#include <pxr/base/tf/status.h>
#include <cassert>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
PXR_NAMESPACE_USING_DIRECTIVE
class Diagnostics: public TfDiagnosticMgr::Delegate {
public:
 std::atomic<int> errors{0},warnings{0};
 void IssueError(TfError const&e) override {++errors;fprintf(stderr,"USD ERROR: %s\n",e.GetCommentary().c_str());}
 void IssueWarning(TfWarning const&e) override {++warnings;fprintf(stderr,"USD WARNING: %s\n",e.GetCommentary().c_str());}
 void IssueStatus(TfStatus const&e) override {fprintf(stderr,"USD STATUS: %s\n",e.GetCommentary().c_str());}
 void IssueFatalError(TfCallContext const&,std::string const&m) override {fprintf(stderr,"USD FATAL: %s\n",m.c_str());_UnhandledAbort();}
};
template<typename Grid>
void check_grid(const std::string& root,const std::string& name,
                typename Grid::ValueType a,typename Grid::ValueType b,HioFormat format){
 auto grid=Grid::create();grid->setName(name);
 grid->setTransform(openvdb::math::Transform::createLinearTransform(.25));
 auto accessor=grid->getAccessor();accessor.setValue({-2,3,4},a);accessor.setValue({-1,3,4},b);
 std::string path=root+"/"+name+".vdb";
 openvdb::io::File file(path);file.write(openvdb::GridPtrVec{grid});file.close();
 auto data=HioFieldTextureData::New(path,name,0,"",0);assert(data&&data->Read());
 assert(data->ResizedWidth()==2&&data->ResizedHeight()==1&&data->ResizedDepth()==1);
 assert(data->GetFormat()==format&&data->HasRawBuffer());
 auto values=reinterpret_cast<const typename Grid::ValueType*>(data->GetRawBuffer());
 assert(values[0]==a&&values[1]==b);
 const auto&box=data->GetBoundingBox();
 assert(box.GetRange().GetMin()==GfVec3d(-2,3,4));
 assert(box.GetRange().GetMax()==GfVec3d(-1,3,4));
 assert(box.GetMatrix().Transform(GfVec3d(4,8,12))==GfVec3d(1,2,3));
 FILE*raw=fopen((root+"/"+name+".raw").c_str(),"wb");assert(raw);
 assert(fwrite(values,sizeof(typename Grid::ValueType),2,raw)==2);assert(fclose(raw)==0);
}
int main(int argc,char**argv){
 Diagnostics diagnostics;TfDiagnosticMgr::GetInstance().AddDelegate(&diagnostics);
 assert(argc==3);std::string root=argv[1],prefix=argv[2];
 auto&registry=PlugRegistry::GetInstance();
 registry.RegisterPlugins(prefix+"/lib/usd/plugInfo.json");
 auto image_plugin=registry.GetPluginForType(TfType::FindByName("HioOIIO_Image"));
 auto volume_plugin=registry.GetPluginForType(TfType::FindByName("HioOpenVDB_TextureData"));
 assert(image_plugin&&image_plugin->GetName()=="hioOiio");
 assert(volume_plugin&&volume_plugin->GetName()=="hioOpenVDB");
 std::string image_path=root+"/hio-roundtrip.tif";
 std::vector<float>pixels(7*5*4);for(size_t i=0;i<pixels.size();i++)pixels[i]=(i%4==3)?1.0f:float(i)/256;
 auto writer=OIIO::ImageOutput::create(image_path);assert(writer);
 OIIO::ImageSpec spec(7,5,4,OIIO::TypeFloat);
 assert(writer->open(image_path,spec));assert(writer->write_image(OIIO::TypeFloat,pixels.data()));assert(writer->close());writer.reset();
 auto image=HioImage::OpenForReading(image_path,0,0,HioImage::Raw);assert(image);
 assert(image->GetWidth()==7&&image->GetHeight()==5);
 HioImage::StorageSpec storage;storage.width=7;storage.height=5;storage.depth=1;storage.format=HioFormatFloat32Vec4;
 std::vector<float>decoded(pixels.size());storage.data=decoded.data();assert(image->Read(storage));
 assert(decoded==pixels);
 openvdb::initialize();auto grid=openvdb::FloatGrid::create();grid->setName("density");
 grid->setTransform(openvdb::math::Transform::createLinearTransform(.5));
 auto accessor=grid->getAccessor();accessor.setValue({1,2,3},1.25f);accessor.setValue({2,2,3},2.5f);
 std::string volume_path=root+"/hio-roundtrip.vdb";openvdb::io::File file(volume_path);openvdb::GridPtrVec grids{grid};file.write(grids);file.close();
 auto volume=HioFieldTextureData::New(volume_path,"density",0,"",128*1024*1024);assert(volume);
 assert(volume->Read());assert(volume->HasRawBuffer());
 assert(volume->ResizedWidth()==2&&volume->ResizedHeight()==1&&volume->ResizedDepth()==1);
 assert(volume->GetFormat()==HioFormatFloat32);
 auto values=reinterpret_cast<const float*>(volume->GetRawBuffer());assert(values[0]==1.25f&&values[1]==2.5f);
 // FieldTextureData uses the statically registered factory directly.
 check_grid<openvdb::FloatGrid>(root,"float",1.25f,2.5f,HioFormatFloat32);
 check_grid<openvdb::DoubleGrid>(root,"double",1.25,2.5,HioFormatDouble64);
 check_grid<openvdb::Vec3fGrid>(root,"vec3f",{1.25f,2.5f,3.75f},{-1.0f,-2.0f,-3.0f},HioFormatFloat32Vec3);
 check_grid<openvdb::Vec3dGrid>(root,"vec3d",{1.25,2.5,3.75},{-1.0,-2.0,-3.0},HioFormatDouble64Vec3);
 auto missing=HioFieldTextureData::New(volume_path,"missing",0,"",0);assert(missing&&!missing->Read());
 assert(volume->Read());
 auto output=HioImage::OpenForWriting(root+"/hio-written.tif");assert(output);
 storage.data=pixels.data();assert(output->Write(storage));output.reset();
 auto written=HioImage::OpenForReading(root+"/hio-written.tif",0,0,HioImage::Raw);assert(written);
 storage.data=decoded.data();assert(written->Read(storage));assert(decoded==pixels);
 assert(image_plugin->IsLoaded());assert(volume_plugin->Load()&&volume_plugin->IsLoaded());
 assert(diagnostics.errors==0&&diagnostics.warnings==1);
 TfDiagnosticMgr::GetInstance().RemoveDelegate(&diagnostics);puts("{\"originalPluginRegistry\":true,\"tiffPixelsExact\":true,\"vdbDenseVoxelsExact\":true,\"gpuRequired\":false,\"volumeTypes\":4,\"volumeTransformsExact\":true,\"missingGridRecovery\":true,\"hioWriteReadExact\":true}");
}
