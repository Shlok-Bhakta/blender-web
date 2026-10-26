/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <OpenImageIO/imageio.h>
#include <libheif/heif.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <string>
int main(int argc,char**argv){
 assert(argc==2);assert(std::string(heif_get_version())=="1.20.2");std::string directory=argv[1];OIIO::attribute("threads",4);
 std::puts("{\"cases\":[");bool first=true;
 for(int bits:{8,10,12})for(int channels:{1,3,4}){
  const int width=37,height=23;std::vector<float>data(width*height*channels);
  for(int y=0;y<height;y++)for(int x=0;x<width;x++)for(int c=0;c<channels;c++)data[(y*width+x)*channels+c]=(c==3)?1.f:float((x*7+y*11+c*31)%256)/255;
  std::string stem=directory+"/oiio-"+std::to_string(bits)+"-"+std::to_string(channels),filename=stem+".avif";
  OIIO::ImageSpec spec(width,height,channels,bits==8?OIIO::TypeUInt8:OIIO::TypeUInt16);spec.attribute("oiio:BitsPerSample",bits);spec.attribute("compression","avif:100");spec.attribute("oiio:UnassociatedAlpha",1);if(channels>1){int cicp[4]={1,13,0,1};spec.attribute("CICP",OIIO::TypeDesc(OIIO::TypeDesc::INT,4),cicp);}
  auto output=OIIO::ImageOutput::create(filename);assert(output);assert(output->open(filename,spec));assert(output->write_image(OIIO::TypeFloat,data.data()));assert(output->close());output.reset();
  OIIO::ImageSpec config;config.attribute("oiio:UnassociatedAlpha",1);auto input=OIIO::ImageInput::open(filename,&config);assert(input);auto decoded_spec=input->spec();assert(decoded_spec.width==width&&decoded_spec.height==height&&decoded_spec.nchannels==channels);assert(decoded_spec.get_int_attribute("oiio:BitsPerSample",8)==bits);
  std::vector<float>decoded(data.size());assert(input->read_image(0,0,0,channels,OIIO::TypeFloat,decoded.data()));assert(input->close());input.reset();
  double max_error=0,mean_error=0;for(size_t i=0;i<data.size();i++){assert(std::isfinite(decoded[i]));double error=std::abs(decoded[i]-data[i]);max_error=std::max(max_error,error);mean_error+=error;}
  if(max_error>(bits==8?1e-6:1.0/((1<<bits)-1)+1e-6)){fprintf(stderr,"roundtrip %d/%d max %.9f\n",bits,channels,max_error);return 2;}
  FILE*raw=fopen((stem+".f32").c_str(),"wb");assert(raw);assert(fwrite(decoded.data(),sizeof(float),decoded.size(),raw)==decoded.size());fclose(raw);
  printf("%s{\"bits\":%d,\"channels\":%d,\"maxError\":%.9f,\"meanError\":%.9f}",first?"":",\n",bits,channels,max_error,mean_error/data.size());first=false;
 }

 std::string bad=directory+"/truncated.avif";
 FILE*file=fopen(bad.c_str(),"wb");assert(file);unsigned char invalid[32]={0};
 assert(fwrite(invalid,1,sizeof(invalid),file)==sizeof(invalid));fclose(file);
 assert(!OIIO::ImageInput::open(bad));assert(!OIIO::geterror().empty());
 auto recovered=OIIO::ImageInput::open(directory+"/oiio-12-4.avif");assert(recovered);assert(recovered->close());
 std::puts("],\"libheifVersion\":\"1.20.2\",\"originalOIIOPlugin\":true,\"oddDimensions\":[37,23],\"invalidInputRejected\":true,\"decoderRecovery\":true}");return 0;
}
