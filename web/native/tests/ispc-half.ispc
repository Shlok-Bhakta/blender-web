// SPDX-License-Identifier: GPL-2.0-or-later
export void half_roundtrip(uniform float input[], uniform float16 storage[], uniform float output[], uniform int count)
{
  foreach (i = 0 ... count) {
    float16 h = (float16)input[i];
    storage[i] = h;
    output[i] = (float)h;
  }
}
export void half_greater(uniform float16 a[], uniform float16 b[], uniform int output[], uniform int count)
{
  foreach (i = 0 ... count) output[i] = a[i] > b[i] ? 1 : 0;
}
