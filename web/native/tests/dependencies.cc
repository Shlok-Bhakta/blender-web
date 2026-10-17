/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <OpenColorIO/OpenColorIO.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/sysutil.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_for.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

static void require(bool condition, const char *message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

int main()
{
  try {
    require(OIIO::Sysutil::physical_memory() > 0, "No Wasm memory budget reported");
    require(OIIO::Sysutil::memory_used() > 0, "No Wasm heap size reported");
    oneapi::tbb::global_control limit(oneapi::tbb::global_control::max_allowed_parallelism, 4);
    std::set<std::thread::id> threads;
    std::mutex mutex;
    std::vector<uint32_t> results(1 << 18);
    oneapi::tbb::parallel_for(oneapi::tbb::blocked_range<size_t>(0, results.size(), 1024),
                             [&](const auto &range) {
      {
        std::lock_guard lock(mutex);
        threads.insert(std::this_thread::get_id());
      }
      for (size_t i = range.begin(); i < range.end(); ++i) {
        uint32_t value = i;
        for (int j = 0; j < 256; ++j) {
          value = value * 1664525u + 1013904223u;
        }
        results[i] = value;
      }
    });
    uint64_t checksum = 0;
    for (size_t i = 0; i < results.size(); ++i) {
      uint32_t expected = i;
      for (int j = 0; j < 256; ++j) {
        expected = expected * 1664525u + 1013904223u;
      }
      require(results[i] == expected, "Parallel scheduler produced incorrect results");
      checksum += results[i];
    }
    require(threads.size() > 1, "oneTBB did not execute on multiple pthreads");
    bool task_exception = false;
    try {
      oneapi::tbb::parallel_for(0, 32, [](int index) {
        if (index == 7) {
          throw std::runtime_error("worker exception");
        }
      });
    }
    catch (const std::runtime_error &) {
      task_exception = true;
    }
    require(task_exception, "oneTBB did not propagate a C++ exception");

    require(OIIO::ustring("ResolutionUnit").string() == "ResolutionUnit",
            "OpenImageIO corrupted an interned string longer than Wasm32 SSO capacity");
    const std::array<unsigned char, 12> pixels = {255, 0, 0, 0, 255, 0, 0, 0, 255, 127, 63, 31};
    auto output = OIIO::ImageOutput::create("/roundtrip.png");
    require(bool(output), "PNG output plugin is unavailable");
    OIIO::ImageSpec specification(2, 2, 3, OIIO::TypeDesc::UINT8);
    specification.attribute("Software", "Blender Web");
    specification.attribute("RenderTime", "00:01.00");
    for (const char *name : {"File", "Date", "Time", "Frame", "FrameRange", "Camera", "Lens", "Scene", "Memory", "Hostname"})
      specification.attribute(name, "Factory scene render");
    specification.attribute("XResolution", 2835);
    specification.attribute("YResolution", 2835);
    specification.attribute("ResolutionUnit", "m");
    require(output->open("/roundtrip.png", specification), "Cannot open PNG output");
    require(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()), "Cannot encode PNG");
    require(output->close(), "Cannot close PNG output");
    auto input = OIIO::ImageInput::open("/roundtrip.png");
    require(bool(input), "Cannot reopen PNG through the virtual filesystem");
    std::array<unsigned char, 12> decoded{};
    require(input->read_image(0, 0, 0, 3, OIIO::TypeDesc::UINT8, decoded.data()), "Cannot decode PNG");
    require(pixels == decoded, "PNG roundtrip changed pixels");
    input->close();
    FILE *valid_file = std::fopen("/roundtrip.png", "rb");
    require(valid_file != nullptr, "Cannot read encoded PNG bytes");
    std::array<unsigned char, 4096> png_bytes{};
    const size_t png_size = std::fread(png_bytes.data(), 1, png_bytes.size(), valid_file);
    std::fclose(valid_file);
    require(png_size > 32, "Encoded PNG is unexpectedly short");
    png_bytes[29] ^= 1;  // Break the IHDR checksum to exercise libpng's longjmp recovery.
    FILE *corrupt_file = std::fopen("/corrupt.png", "wb");
    require(corrupt_file != nullptr, "Cannot create corrupt PNG fixture");
    require(std::fwrite(png_bytes.data(), 1, png_size, corrupt_file) == png_size, "Cannot write corrupt PNG fixture");
    std::fclose(corrupt_file);
    require(!OIIO::ImageInput::open("/corrupt.png"), "PNG decoder accepted a corrupt header");

    namespace OCIO = OCIO_NAMESPACE;
    auto config = OCIO::Config::CreateRaw();
    auto processor = config->getProcessor("raw", "raw")->getDefaultCPUProcessor();
    float color[3] = {0.25f, 0.5f, 0.75f};
    processor->applyRGB(color);
    require(color[0] == 0.25f && color[1] == 0.5f && color[2] == 0.75f, "Raw color transform changed pixels");
    bool color_exception = false;
    try {
      config->getProcessor("missing-color-space", "raw");
    }
    catch (const OCIO::Exception &) {
      color_exception = true;
    }
    require(color_exception, "OpenColorIO did not propagate a C++ exception");

    FT_Library fonts;
    require(FT_Init_FreeType(&fonts) == 0, "Cannot initialize FreeType");
    FT_Face face;
    require(FT_New_Face(fonts, "/Inter.woff2", 0, &face) == 0, "Cannot decode Blender's Brotli-compressed font");
    require(FT_Get_Char_Index(face, 'B') != 0, "Blender font has no B glyph");
    FT_Done_Face(face);
    FT_Done_FreeType(fonts);

    std::printf("{\"threads\":%zu,\"checksum\":%llu,\"pngRoundtrip\":true,\"pngMetadata\":true,\"colorTransform\":true,\"woff2\":true,\"exceptions\":true,\"longjmp\":true}\n",
                threads.size(), static_cast<unsigned long long>(checksum));
    return 0;
  }
  catch (const std::exception &error) {
    std::fprintf(stderr, "Native dependency validation failed: %s\n", error.what());
    return 1;
  }
}
