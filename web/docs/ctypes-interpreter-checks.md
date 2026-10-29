# Original ctypes interpreter checks

CPython 3.13.13's original, unchanged _ctypes sources now run with libffi 3.5.2 in an isolated Wasm interpreter. Node and Chromium pass the same checks as installed native Blender 5.2.2 LTS, including NumPy 2.3.4 interoperability. Python finalization completes with no remaining registered closures. This has not yet been integrated into the production Blender application.

The tests cover exact 64-bit integers above 53 bits, aggregate arguments and returns, long doubles, mixed variadic calls, memory operations, the real process namespace, missing-symbol rejection, errno, Python API exceptions and reference counts. NumPy shares ctypes array views, calls through ndpointer and rejects a noncontiguous array before recovering on a valid call.

Four existing C++ workers invoke newly created Python callbacks 240 times across 60 signature changes. Eight Python threads call another callback. No unraisable errors or browser errors occur. Native Blender runs the same Python script against the same independently compiled C fixture, using its own original ctypes/libffi. Pointer and long sizes are explicitly 4 bytes in Wasm32 and 8 in native x86-64; the remaining reports match exactly.

The worker bridge adds no periodic polling. It also now handles a callback that exits its thread while another worker waits to free it. Skipping that active-counter cleanup reproduces a stuck retirement. The full 1,000-cycle library stress check and extended ABI suite pass in Node and Chromium with that fix.

The isolated loader registers a small real static process namespace and rejects named dynamic libraries. General native or dynamic Wasm library loading is unsupported. Application startup registration, cancellation, other lifecycle cases and application performance still need work. This is not full dependency parity.

See the [exact results and hashes](ctypes-interpreter-checks.json), [source, logs and reproduction instructions](ctypes-interpreter-source.tar.gz), and [compiled test bundles](https://github.com/Shlok-Bhakta/blender-web/releases/download/web-native-f5bc7678d055b2921075/ctypes-interpreter-binaries.tar.gz). The earlier [worker protocol checks](libffi-events-checks.md) explain deferred installation and the two negative controls.
