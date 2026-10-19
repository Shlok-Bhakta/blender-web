/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fftw3.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static void require(bool value)
{
  if (!value) std::abort();
}

int main()
{
  constexpr int n = 4096;
  require(fftw_init_threads() && fftwf_init_threads());
  fftw_plan_with_nthreads(3);
  fftwf_plan_with_nthreads(3);
  auto *d = fftw_alloc_complex(n);
  auto *f = fftwf_alloc_complex(n);
  require(d && f);
  for (int i = 0; i < n; i++) {
    d[i][0] = f[i][0] = (i == 0 ? 1 : 0);
    d[i][1] = f[i][1] = 0;
  }
  auto dp = fftw_plan_dft_1d(n, d, d, FFTW_FORWARD, FFTW_ESTIMATE);
  auto fp = fftwf_plan_dft_1d(n, f, f, FFTW_FORWARD, FFTW_ESTIMATE);
  require(dp && fp);
  fftw_execute(dp);
  fftwf_execute(fp);
  for (int i = 0; i < n; i++) {
    require(std::abs(d[i][0] - 1) < 1e-12 && std::abs(d[i][1]) < 1e-12);
    require(std::abs(f[i][0] - 1) < 1e-6 && std::abs(f[i][1]) < 1e-6);
  }
  fftw_destroy_plan(dp);
  fftwf_destroy_plan(fp);
  fftw_free(d);
  fftwf_free(f);
  fftw_cleanup_threads();
  fftwf_cleanup_threads();
  std::puts("{\"fftwDouble\":true,\"fftwFloat\":true,\"transformSize\":4096,\"requestedThreads\":3}");
}
