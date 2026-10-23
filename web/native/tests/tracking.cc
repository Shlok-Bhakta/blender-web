/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <cassert>
#include <cmath>
#include <cstdio>
#include <ceres/ceres.h>
#include <ceres/version.h>

struct Projection {
  double x, y, z, u, v;
  template<typename T> bool operator()(const T *offset, T *residual) const
  {
    const T depth = T(z) - offset[2];
    residual[0] = T(220) * (T(x) - offset[0]) / depth - T(u);
    residual[1] = T(220) * (T(y) - offset[1]) / depth - T(v);
    return true;
  }
};

int main()
{
  const double expected[] = {1.25, -.5, .3};
  double maximum = 0;
  for (const auto solver : {ceres::DENSE_QR, ceres::SPARSE_NORMAL_CHOLESKY}) {
    double offset[] = {0, 0, 0};
    ceres::Problem problem;
    for (int i = 0; i < 32; i++) {
      const double x = ((i * 37) % 23 - 11) / 8.0;
      const double y = ((i * 19) % 17 - 8) / 7.0;
      const double z = 4 + ((i * 13) % 19) / 5.0;
      auto *projection = new Projection{x, y, z,
          220 * (x - expected[0]) / (z - expected[2]),
          220 * (y - expected[1]) / (z - expected[2])};
      problem.AddResidualBlock(new ceres::AutoDiffCostFunction<Projection, 2, 3>(projection),
                               nullptr, offset);
    }
    ceres::Solver::Options options;
    options.linear_solver_type = solver;
    options.sparse_linear_algebra_library_type = ceres::EIGEN_SPARSE;
    options.num_threads = 8;
    options.max_num_iterations = 50;
    options.function_tolerance = 1e-12;
    options.parameter_tolerance = 1e-12;
    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
    assert(summary.IsSolutionUsable() && summary.final_cost < 1e-12);
    assert(summary.linear_solver_type_used == solver && summary.num_threads_used == 8);
    for (int i = 0; i < 3; i++) maximum = std::max(maximum, std::abs(offset[i] - expected[i]));
  }
  assert(maximum < 1e-7);
  std::printf("{\"ceres\":\"%s\",\"denseQR\":true,\"eigenSparse\":true,"
              "\"threads\":8,\"maximumParameterError\":%.12g}\n", CERES_VERSION_STRING, maximum);
}
