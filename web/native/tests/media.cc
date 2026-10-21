/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <sndfile.h>
#include <x264.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/cpu.h>
#include <libswscale/swscale.h>
}

int main()
{
  assert(av_get_cpu_flags() & AV_CPU_FLAG_SIMD128);
  for (const char *name : {"libx264", "libvpx-vp9", "libopus", "aac", "libmp3lame", "ffv1"}) {
    assert(avcodec_find_encoder_by_name(name));
  }
  SF_INFO info{};
  info.samplerate = 48000;
  info.channels = 2;
  info.format = SF_FORMAT_FLAC | SF_FORMAT_PCM_16;
  std::vector<short> samples(4096);
  for (int i = 0; i < 2048; i++) {
    samples[i * 2] = short(12000 * std::sin(i * 0.1));
    samples[i * 2 + 1] = -samples[i * 2];
  }
  SNDFILE *file = sf_open("/roundtrip.flac", SFM_WRITE, &info);
  assert(file);
  assert(sf_writef_short(file, samples.data(), 2048) == 2048);
  assert(sf_close(file) == 0);
  info = {};
  file = sf_open("/roundtrip.flac", SFM_READ, &info);
  assert(file && info.frames == 2048 && info.channels == 2 && info.samplerate == 48000);
  std::vector<short> decoded(samples.size());
  assert(sf_readf_short(file, decoded.data(), 2048) == 2048 && decoded == samples);
  sf_close(file);

  AVFormatContext *format = nullptr;
  assert(avformat_open_input(&format, "/reference.mkv", nullptr, nullptr) == 0);
  assert(avformat_find_stream_info(format, nullptr) >= 0);
  const AVCodec *decoder = nullptr;
  const int stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  assert(stream >= 0 && decoder);
  AVCodecContext *codec = avcodec_alloc_context3(decoder);
  assert(avcodec_parameters_to_context(codec, format->streams[stream]->codecpar) == 0);
  codec->thread_count = 4;
  assert(avcodec_open2(codec, decoder, nullptr) == 0);
  assert(codec->thread_count == 4 && codec->active_thread_type != 0);
  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  unsigned frames = 0;
  std::vector<unsigned char> rgb(128 * 96 * 3);
  auto receive = [&] {
    while (avcodec_receive_frame(codec, frame) == 0) {
      assert(frame->width == 128 && frame->height == 96);
      SwsContext *scale = sws_getContext(128, 96, AVPixelFormat(frame->format),
          128, 96, AV_PIX_FMT_RGB24, SWS_POINT, nullptr, nullptr, nullptr);
      assert(scale);
      unsigned char *output[] = {rgb.data()};
      const int stride[] = {128 * 3};
      assert(sws_scale(scale, frame->data, frame->linesize, 0, 96, output, stride) == 96);
      for (int channel = 0; channel < 3; channel++) {
        assert(rgb[(48 * 128 + 64) * 3 + channel] == (frames * (channel == 0 ? 17 : channel == 1 ? 37 : 61)) % 256);
      }
      sws_freeContext(scale);
      frames++;
    }
  };
  while (av_read_frame(format, packet) >= 0) {
    if (packet->stream_index == stream) {
      assert(avcodec_send_packet(codec, packet) == 0);
      receive();
    }
    av_packet_unref(packet);
  }
  assert(avcodec_send_packet(codec, nullptr) == 0);
  receive();
  assert(frames == 24);
  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec);
  avformat_close_input(&format);
  x264_param_t parameters;
  assert(x264_param_default_preset(&parameters, "ultrafast", nullptr) == 0);
  parameters.i_width = 128;
  parameters.i_height = 720;
  parameters.i_log_level = X264_LOG_ERROR;
  parameters.i_threads = X264_THREADS_AUTO;
  x264_t *encoder = x264_encoder_open(&parameters);
  assert(encoder);
  x264_encoder_parameters(encoder, &parameters);
  const int encoder_threads = parameters.i_threads;
  assert(encoder_threads >= 1 && (av_cpu_count() <= 1 || encoder_threads > 1));
  x264_encoder_close(encoder);
  std::printf("{\"videoFrames\":%u,\"decoderThreads\":4,\"automaticEncoderThreads\":%d,\"cpuCores\":%d,\"flacSamplesExact\":4096,\"ffmpegVersion\":\"%s\"}\n", frames, encoder_threads, av_cpu_count(), av_version_info());
}
