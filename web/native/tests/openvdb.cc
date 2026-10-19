/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>

static void require(bool condition, const char *message)
{
  if (!condition) throw std::runtime_error(message);
}

int main(int argc, char **argv)
{
  try {
    require(argc == 2, "Expected output fixture directory");
    openvdb::initialize();
    auto grid = openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(
        1.0f, openvdb::Vec3f(0.0f), 0.125f, 3.0f);
    grid->setName("surface");
    const auto count = grid->activeVoxelCount();
    require(count > 1000, "Sphere is empty");
    auto handle = nanovdb::tools::createNanoGrid(*grid);
    const auto *nano = handle.grid<float>();
    require(nano && nano->activeVoxelCount() == count, "NanoVDB voxel count mismatch");
    auto accessor = nano->getAccessor();
    double maximum_error = 0;
    for (auto it = grid->cbeginValueOn(); it; ++it) {
      const auto p = it.getCoord();
      maximum_error = std::max(maximum_error,
          double(std::abs(*it - accessor.getValue(nanovdb::Coord(p.x(), p.y(), p.z())))));
    }
    require(maximum_error == 0, "NanoVDB changed voxel values");
    auto fog = grid->deepCopy();
    openvdb::tools::sdfToFogVolume(*fog);
    fog->setName("density");
    for (const auto compression : {openvdb::io::COMPRESS_ZIP, openvdb::io::COMPRESS_BLOSC}) {
      const auto name = std::string(argv[1]) +
          (compression == openvdb::io::COMPRESS_ZIP ? "/sphere-zip.vdb" : "/sphere-blosc.vdb");
      openvdb::io::File file(name);
      file.setCompression(compression | openvdb::io::COMPRESS_ACTIVE_MASK);
      file.write(openvdb::GridPtrVec{grid, fog});
      file.close();
      file.open();
      for (const auto &original : {grid, fog}) {
        const auto restored = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(original->getName()));
        require(restored && restored->activeVoxelCount() == original->activeVoxelCount(), "VDB IO voxel count mismatch");
        for (auto it = original->cbeginValueOn(); it; ++it) {
          require(*it == restored->tree().getValue(it.getCoord()), "VDB IO changed voxel values");
        }
      }
      file.close();
    }
    std::printf("{\"activeVoxels\":%llu,\"nanoMaximumError\":%.9g,"
                "\"zipRoundtrip\":true,\"bloscRoundtrip\":true,\"pointerBytes\":%zu}\n",
                static_cast<unsigned long long>(count), maximum_error, sizeof(void *));
    return 0;
  }
  catch (const std::exception &error) {
    std::fprintf(stderr, "OpenVDB validation failed: %s\n", error.what());
    return 1;
  }
}
