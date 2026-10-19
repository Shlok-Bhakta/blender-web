/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>
#include <cstdio>
#include <cstdlib>

namespace A = Alembic::Abc;
namespace G = Alembic::AbcGeom;

static void require(bool value)
{
  if (!value) std::abort();
}

int main()
{
  Alembic::AbcCoreFactory::IFactory factory;
  factory.setPolicy(A::ErrorHandler::kThrowPolicy);
  factory.setOgawaNumStreams(4);
  auto desktop = factory.getArchive("/desktop-ocean.abc");
  require(desktop.valid());
  G::IXform transform(desktop.getTop(), "NativeOcean");
  G::IPolyMesh ocean(transform, transform.getChildHeader(0).getName());
  require(ocean.getSchema().getNumSamples() == 12);
  auto first = ocean.getSchema().getValue(A::ISampleSelector(A::index_t(0)));
  auto last = ocean.getSchema().getValue(A::ISampleSelector(A::index_t(11)));
  require(first.getPositions()->size() == 676 && last.getPositions()->size() == 676);
  require(first.getFaceCounts()->size() == 625);
  require((*first.getPositions())[0] != (*last.getPositions())[0]);
  {
    A::OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(), "/roundtrip.abc");
    G::OPolyMesh mesh(archive.getTop(), "Ocean");
    for (const auto &sample : {first, last}) {
      mesh.getSchema().set(G::OPolyMeshSchema::Sample(
        *sample.getPositions(), *sample.getFaceIndices(), *sample.getFaceCounts()));
    }
  }
  auto written = factory.getArchive("/roundtrip.abc");
  G::IPolyMesh mesh(written.getTop(), "Ocean");
  require(mesh.getSchema().getNumSamples() == 2);
  auto sample = mesh.getSchema().getValue(A::ISampleSelector(A::index_t(1)));
  require(sample.getPositions()->size() == 676);
  require((*sample.getPositions())[0] == (*last.getPositions())[0]);
  std::puts("{\"alembicOgawa\":true,\"desktopAnimationSamples\":12,\"vertices\":676,\"faces\":625,\"writeRead\":true,\"wasmFS\":true}");
}
