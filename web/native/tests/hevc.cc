// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <x265.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}
static unsigned sample(int x,int y,int plane,int frame,int depth){return unsigned(x*19+y*37+plane*113+frame*67)&((1u<<depth)-1);}
int main(int argc,char**argv){
 av_log_set_level(AV_LOG_ERROR);printf("{\"bitDepths\":[");
 for(int depth:{8,10,12}){
  const x265_api* api=x265_api_get(depth);assert(api&&api->bit_depth==depth);
  const AVCodec* encoder=avcodec_find_encoder_by_name("libx265");assert(encoder);
  auto enc=avcodec_alloc_context3(encoder);enc->width=96;enc->height=64;enc->time_base={1,24};enc->framerate={24,1};
  enc->pix_fmt=depth==8?AV_PIX_FMT_YUV420P:depth==10?AV_PIX_FMT_YUV420P10LE:AV_PIX_FMT_YUV420P12LE;
  assert(av_opt_set(enc->priv_data,"preset","ultrafast",0)==0);
  assert(av_opt_set(enc->priv_data,"x265-params","lossless=1:bframes=0:frame-threads=2:pools=2:log-level=error",0)==0);
  assert(avcodec_open2(enc,encoder,nullptr)==0);
  auto dec=avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_HEVC));assert(dec);dec->thread_count=2;
  assert(avcodec_open2(dec,nullptr,nullptr)==0);
  AVFrame* input=av_frame_alloc();input->format=enc->pix_fmt;input->width=96;input->height=64;assert(av_frame_get_buffer(input,32)==0);
  AVFrame* frame=av_frame_alloc();AVPacket* packet=av_packet_alloc();int decoded=0;size_t bytes=0;
  FILE* file=nullptr;if(argc>1){std::string name=std::string(argv[1])+"-"+std::to_string(depth)+".hevc";file=fopen(name.c_str(),"wb");assert(file);}
  auto receiveFrames=[&](){int status;while((status=avcodec_receive_frame(dec,frame))==0){
   assert(decoded<3&&frame->width==96&&frame->height==64);assert(av_pix_fmt_desc_get(AVPixelFormat(frame->format))->comp[0].depth==depth);
   for(int p=0;p<3;p++)for(int y=0;y<(64>>(p?1:0));y++)for(int x=0;x<(96>>(p?1:0));x++){
    const uint8_t* row=frame->data[p]+y*frame->linesize[p];unsigned value=depth==8?row[x]:reinterpret_cast<const uint16_t*>(row)[x];assert(value==sample(x,y,p,decoded,depth));
   }++decoded;av_frame_unref(frame);
  }assert(status==AVERROR(EAGAIN)||status==AVERROR_EOF);};
  auto receivePackets=[&](){int status;while((status=avcodec_receive_packet(enc,packet))==0){
   if(file)assert(fwrite(packet->data,1,packet->size,file)==size_t(packet->size));bytes+=packet->size;
   assert(avcodec_send_packet(dec,packet)==0);receiveFrames();av_packet_unref(packet);
  }assert(status==AVERROR(EAGAIN)||status==AVERROR_EOF);};
  for(int i=0;i<3;i++){
   assert(av_frame_make_writable(input)==0);input->pts=i;
   for(int p=0;p<3;p++)for(int y=0;y<(64>>(p?1:0));y++)for(int x=0;x<(96>>(p?1:0));x++){
    uint8_t* row=input->data[p]+y*input->linesize[p];unsigned value=sample(x,y,p,i,depth);if(depth==8)row[x]=value;else reinterpret_cast<uint16_t*>(row)[x]=value;
   }assert(avcodec_send_frame(enc,input)==0);receivePackets();
  }
  assert(avcodec_send_frame(enc,nullptr)==0);receivePackets();assert(avcodec_send_packet(dec,nullptr)==0);receiveFrames();assert(decoded==3&&bytes>0);
  if(file)fclose(file);av_frame_free(&input);av_frame_free(&frame);av_packet_free(&packet);avcodec_free_context(&enc);avcodec_free_context(&dec);
  if(depth!=8)printf(",");printf("{\"depth\":%d,\"frames\":%d,\"bytes\":%zu,\"losslessExact\":true}",depth,decoded,bytes);
 }
 printf("]}\n");
}
