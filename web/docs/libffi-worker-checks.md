# Generic libffi worker checks

An isolated libffi 3.5.2 prototype passed 1,000 callback signature changes and 9,332 exact calls. This is a Node pthread Wasm experiment. It is not linked into the production Blender build, and browser validation remains pending.

The test uses the original generic CIF marshaller and typed trampoline generator. Four signature categories cover integers, 64-bit values above 53 bits mixed with doubles, aggregate arguments and returns, and true variadic floating-point arguments. Eight existing workers, 250 new workers and four concurrent publishers passed. Table reuse stayed bounded at 87 entries, with 84 during serial reuse. This does not prove bounded compiled-module or garbage-collector memory.

Callback retirement waits for a running callback. A new test also creates, invokes and frees another callback from inside it while another worker waits to free the first. Holding the registry lock during that wait causes a deadlock. The same binary reproduces it with the `locked-free` control; releasing the lock during retirement passes the expanded stress test. Disabling worker propagation separately reproduces a missing table entry. Explicit pthread_exit cleanup also passed.

An independent C driver resolved libffi from pinned native Blender 5.2.2 LTS's original `_ctypes` module. Its 720 exact calls include 640 worker calls using the same four signature categories. The native module, driver, Wasm sources, binaries and logs have recorded hashes.

The current bridge polls futex waits in two-millisecond slices while preserving the original deadline. Replacing polling, testing forced termination and cancellation, self-free, stale callback calls, untracked workers, more ABI layouts and full application ctypes integration remain necessary. This is not evidence of full dependency parity.

The [machine-readable proof](libffi-worker-checks.json) identifies the exact inputs and limitations. The [80 KiB evidence archive](libffi-worker-probe-evidence.tar.gz) contains the source patch, C++ drivers, binaries, logs and reproduction instructions.

The [extended argument checks](libffi-varargs-checks.md) add mixed varargs, long doubles, small aggregates and zero fixed arguments. They fix a separate forward-marshalling alignment bug and retain the full worker stress result.
