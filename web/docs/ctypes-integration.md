# Original ctypes integration

The working source builds CPython 3.13.13's original `_ctypes` sources against
Blender's pinned libffi 3.5.2. The first integrated Blender build passed in
hardware Chromium, repeated its checks after an offline reload, and matched
native Blender 5.2.2 LTS. The public Pages release remains
`f5bc7678d055b2921075` while the new candidate completes validation.

The module can call 1,260 Python symbols and nine explicitly registered libc
symbols through the static process namespace. It supports Python API calls,
data symbols, structs, 64-bit integers, variadic formatting, NumPy ctypes views,
and callbacks on existing and newly created pthreads. Named external libraries
and arbitrary dynamic Wasm extensions remain unsupported.

The implementation is owned by GHOST. A forced header redirects loader calls
only in the original `_ctypes` translation units. The rest of Blender retains
its existing loader behavior. The symbol table takes addresses of actual typed
declarations found in the installed Python headers and static library.

`GHOST_WebFFI.cc` registers pthreads before their entry functions run and installs
libffi's original typed trampolines in each worker's function table. Active
workers receive coalesced notices. Sleeping workers reconcile when their
existing futex wait returns. Retirement waits for running callbacks without
holding the registry mutex. Thread cleanup releases active counts when a
callback calls `pthread_exit`. The bridge adds no periodic polling; the SDK's
existing waits retain their own behavior.
Publication waits for active registered workers to reach a proxy or wait
boundary. A long C job can delay callback creation.

The dependency patch corrects mixed variadic argument layout and unsigned
address handling in libffi's Wasm backend. It also permits installation in
another worker without rewriting shared closure metadata. Blender's original
source files require no additional edits for this integration.

Verified dependency checks in Node and Chromium:

- 9,341 calls, 1,000 signature changes, eight existing workers, 80 concurrent
  closures, nested creation during retirement, and thread exit inside a callback.
- A variadic callback with no extra arguments.
- Sparse heap growth to 2,418,016,256 bytes, CIF metadata above 2 GiB, and struct
  calls and callbacks using a stack above 2 GiB.
- A controlled closure allocation above 2 GiB, including metadata initialization,
  preparation, invocation and retirement. The original allocation path fails
  the metadata check; normalizing its returned pointer fixes it.

The integrated application checks use real Python and libc APIs rather than a
replacement ctypes implementation. They exercise 240 callbacks across four
existing workers and eight callbacks on new Python threads. Native Blender
matches all functional results; pointer and C `long` widths differ as expected
between wasm32 and Linux x86-64.

The high-address check reserves virtual memory sparsely. It does not establish
multi-gigabyte Blender scene capacity. The application still uses a wasm32 heap
limited to 4 GiB. Callback lifetime must cover every foreign caller. Freeing a
callback from inside itself and re-preparing a live closure are unsupported.

Build and run the checks with the pinned SDK activated:

```sh
python3 web/tools/native_build.py --app --check-deps --jobs 4
node web/tests/ffi-workers.cjs
WEB_TEST_HARDWARE=1 node web/tests/python-ctypes.cjs
python3 web/tools/native_ctypes_verify.py build/native/python-ctypes-evidence-hardware
```

The full validator includes hardware, software and fallback browser variants,
native comparisons, offline repeats and the hardware recording. The Pages
publisher includes a separate ctypes group for both application variants.
