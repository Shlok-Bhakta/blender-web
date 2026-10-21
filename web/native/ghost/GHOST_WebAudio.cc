/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "GHOST_WebAudio.hh"

#ifdef WITH_AUDASPACE
#  include "devices/DeviceManager.h"
#  include "devices/IDeviceFactory.h"
#  include "devices/SoftwareDevice.h"
#  include <emscripten.h>
#  include <atomic>
#  include <chrono>
#  include <cstddef>
#  include <thread>

namespace {
constexpr unsigned capacity = 8192;
constexpr unsigned chunk = 256;
// Single producer and single consumer. Only the mixer writes `write`; only
// the AudioWorklet writes `read`. Publish PCM before its release-store index.
struct AudioRing {
  std::atomic<unsigned> read{0}, write{0}, ready{0}, underruns{0};
  std::atomic<unsigned> consumed{0}, peak{0}, nonzero{0}, closed{0};
  std::atomic<unsigned> requested{0};
  float samples[capacity * 2]{};
};
static_assert(sizeof(std::atomic<unsigned>) == 4);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(offsetof(AudioRing, samples) == 36);

class WebAudioDevice final : public aud::SoftwareDevice {
  AudioRing *ring_ = new AudioRing;
  std::atomic<bool> quit_{false};
  std::atomic<bool> playing_{false};
  std::thread mixer_;

  void playing(bool value) override
  {
    playing_.store(value);
    ring_->requested.store(value ? 1 : 0);
  }

 public:
  WebAudioDevice()
  {
    m_specs.channels = aud::CHANNELS_STEREO;
    m_specs.format = aud::FORMAT_FLOAT32;
    m_specs.rate = static_cast<aud::SampleRate>(MAIN_THREAD_EM_ASM_INT({
      return globalThis.blenderWebAudioContext().sampleRate;
    }));
    create();
    MAIN_THREAD_EM_ASM({
      globalThis.blenderWebAudioAttach(HEAPU8.buffer, $0, $1);
    }, ring_, capacity);
    mixer_ = std::thread([this] {
      while (!quit_.load()) {
        const unsigned write = ring_->write.load(std::memory_order_relaxed);
        const unsigned read = ring_->read.load(std::memory_order_acquire);
        // Keep at most 2048 frames queued. The ring is larger to make index
        // wrap harmless and leave the real-time consumer independent of locks.
        if (playing_.load() && ring_->ready.load() == 1 && write - read < 2048) {
          mix(reinterpret_cast<aud::data_t *>(ring_->samples + (write % capacity) * 2), chunk);
          ring_->write.store(write + chunk, std::memory_order_release);
        }
        else {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      }
    });
  }

  ~WebAudioDevice() override
  {
    quit_.store(true);
    mixer_.join();
    destroy();
    ring_->closed.store(1);
    MAIN_THREAD_EM_ASM({ globalThis.blenderWebAudioDetach($0); }, ring_);
    // Detach disconnects the worklet before acknowledging the allocation.
    // Free on its asynchronous completion, never while process() can read it.
  }
};

class WebAudioFactory final : public aud::IDeviceFactory {
 public:
  std::shared_ptr<aud::IDevice> openDevice() override
  {
    return std::make_shared<WebAudioDevice>();
  }
  int getPriority() override { return 1 << 10; }
  void setSpecs(aud::DeviceSpecs) override {}
  void setBufferSize(int) override {}
  void setName(const std::string &) override {}
};
} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE void blender_web_audio_release(AudioRing *ring)
{
  delete ring;
}

void GHOST_web_register_audio()
{
  aud::DeviceManager::registerDevice("Web Audio", std::make_shared<WebAudioFactory>());
}
#endif
