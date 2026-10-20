/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <Python.h>
#include <blender-web-usd.hh>
#include <pxr/imaging/hd/meshTopology.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usdImaging/usdImaging/sphereAdapter.h>
int main()
{
  GHOST_web_register_usd();
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  PyConfig_SetBytesString(&config, &config.home, "/python");
  PyStatus status = Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status)) Py_ExitStatusException(status);
  {
    auto stage = pxr::UsdStage::CreateInMemory();
    auto sphere = pxr::UsdGeomSphere::Define(stage, pxr::SdfPath("/Sphere"));
    sphere.GetRadiusAttr().Set(2.0);
    pxr::UsdImagingSphereAdapter adapter;
    const auto points = adapter.GetPoints(sphere.GetPrim(), pxr::UsdTimeCode::Default());
    const auto topology = adapter.GetTopology(
        sphere.GetPrim(), pxr::SdfPath(), pxr::UsdTimeCode::Default());
    if (!points.IsHolding<pxr::VtVec3fArray>() ||
        points.UncheckedGet<pxr::VtVec3fArray>().size() != 92 ||
        !topology.IsHolding<pxr::HdMeshTopology>() ||
        topology.UncheckedGet<pxr::HdMeshTopology>().GetFaceVertexCounts().size() != 100)
      return 3;
  }
  const int result = PyRun_SimpleString(
      "import json\n"
      "from pxr import Tf, Gf, Vt, Sdf, Sdr, Usd, UsdGeom, UsdShade, UsdMtlx, Plug\n"
      "stage = Usd.Stage.CreateNew('/tmp/static-usd.usdc')\n"
      "mesh = UsdGeom.Mesh.Define(stage, '/Cube')\n"
      "mesh.CreatePointsAttr(Vt.Vec3fArray([Gf.Vec3f(0,0,0), Gf.Vec3f(1,0,0), Gf.Vec3f(0,1,0)]))\n"
      "mesh.CreateFaceVertexCountsAttr([3])\n"
      "mesh.CreateFaceVertexIndicesAttr([0,1,2])\n"
      "stage.GetRootLayer().Save()\n"
      "reopened = Usd.Stage.Open('/tmp/static-usd.usdc')\n"
      "assert len(UsdGeom.Mesh(reopened.GetPrimAtPath('/Cube')).GetPointsAttr().Get()) == 3\n"
      "assert Plug.Registry().GetPluginWithName('usdMtlx')\n"
      "assert Plug.Registry().GetPluginWithName('usdShaders')\n"
      "assert 'UsdPreviewSurface' in Sdr.Registry().GetShaderNodeIdentifiers()\n"
      "assert Sdr.Registry().GetShaderNodeByIdentifier('UsdPreviewSurface')\n"
      "tet = UsdGeom.TetMesh.Define(stage, '/Tetrahedra')\n"
      "tet.CreateTetVertexIndicesAttr(Vt.Vec4iArray([Gf.Vec4i(0,1,2,3), Gf.Vec4i(0,1,2,4)]))\n"
      "faces = UsdGeom.TetMesh.ComputeSurfaceFaces(tet)\n"
      "assert len(faces) == 6 and all(set(face) != {0,1,2} for face in faces)\n"
      "print(json.dumps({'usdVersion': Usd.GetVersion(), 'staticPythonBindings': True, 'usdcRoundtrip': True, 'materialXPlugin': True, 'tetMeshSurfaceFaces': len(faces), 'sphereAdapterPoints': 92}))\n");
  return result || Py_FinalizeEx();
}
