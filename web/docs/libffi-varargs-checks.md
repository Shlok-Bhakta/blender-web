# Extended libffi argument checks

The isolated original libffi 3.5.2 worker prototype now passes 72 additional callback calls and nine calls to a normal compiler-generated variadic function. All four categories pass both direct callback invocation and ffi_call. Native Blender's own libffi passes the same inputs and exact expected results.

The cases cover mixed variadic integers, doubles, a 64-bit value above 53 bits, pointers, an aggregate, long double and a trailing integer; fixed long-double arguments and return; single-field aggregate varargs; and zero fixed arguments. The compiler-generated function reads the values through va_arg, checking the forward marshaller independently of libffi's callback decoder.

The expanded test exposed another upstream Wasm marshalling bug. Allocating each argument backwards inserted some alignment padding after an argument instead of before the next. A trailing integer passed as 8 arrived as 29. The fix calculates forward argument offsets before reserving an aligned buffer. The archived pre-fix binary reproduces that failure. The corrected backend also passes the earlier 1,000-cycle, 9,332-call worker stress test, including concurrent creation, nested creation during retirement and pthread_exit cleanup.

These are Node pthread tests. Browser validation and production ctypes integration remain pending. That snapshot's worker bridge polls every two milliseconds. Cancellation, forced termination, self-free, stale callback calls, more ABI layouts and resource lifetime need further work.

See the [exact proof and limitations](libffi-varargs-checks.json), [source, binaries, logs and reproduction instructions](libffi-varargs-probe-evidence.tar.gz), and [earlier worker lifecycle checks](libffi-worker-checks.md).

The [successor worker protocol](libffi-events-checks.md) retains these marshalling fixes and passes the extended ABI suite in Chromium without bridge polling.
