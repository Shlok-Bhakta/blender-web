# Callback workers without bridge polling

The isolated original libffi 3.5.2 prototype now passes in Node and real Chromium without adding periodic polling to worker waits. Each runs 1,000 signature changes and 9,340 exact callback calls. Four concurrent publishers, nested callback creation during retirement, in-flight freeing and explicit pthread_exit cleanup also pass. This is not yet linked into production Blender or its ctypes module.

Eight workers wait indefinitely. They perform no repeated physical waits over 100 ms and queue no notices during 200 callback publication/retirement pairs. A final callback is then published, and all eight workers install it before returning to user code. Restoring two-millisecond polling fails the idle-count assertion. Skipping wake-time installation causes the expected table trap. Both controls use the same executable.

A waking worker reconciles the callback registry before returning from its original futex wait. Active workers acknowledge publications. Sleeping workers can defer installation until they wake, and notices are coalesced. Retirement waits on a signalled progress word. The SDK still has its own 100 ms condition-variable wait slices; the raw-futex idle test distinguishes these from bridge polling.

The extended ABI suite also passes 72 callback calls and nine calls to a compiler-generated variadic function in Node and Chromium, with exact native Blender readback. It covers mixed varargs, long doubles, aggregate arguments and zero fixed arguments. The prior marshalling fixes remain in the backend.

The Node stress run completed in 3.78 seconds on this host. That is a functional test duration, not an application render benchmark. The registry mutex surrounds wait transitions, so application performance still needs measurement. Full worker registration, cancellation, thread exit during a callback, self-free, stale calls and resource lifetime remain unresolved or unverified. Bounded table reuse does not prove bounded compiled-module memory.

See the [source and result hashes](libffi-events-checks.json), [complete evidence and reproduction instructions](libffi-events-probe-evidence.tar.gz), and [earlier argument checks](libffi-varargs-checks.md).
