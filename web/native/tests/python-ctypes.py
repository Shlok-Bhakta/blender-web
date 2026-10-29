# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the original ctypes module using actual Python and libc symbols."""
def run():
    import ctypes as c
    import gc
    import sys
    import threading
    import numpy as np

    errors = []
    original_hook = sys.unraisablehook
    sys.unraisablehook = lambda value: errors.append(str(value.exc_value))
    try:
        process = c.CDLL(None)
        process.strlen.argtypes = [c.c_char_p]
        process.strlen.restype = c.c_size_t
        assert process.strlen(b'Blender ctypes') == 14
        try:
            process.this_symbol_does_not_exist_in_blender_web
        except AttributeError:
            pass
        else:
            raise AssertionError('Missing process symbol was accepted')
        assert c.py_object.in_dll(process, 'PyExc_ValueError').value is ValueError
        c.pythonapi.Py_GetVersion.restype = c.c_char_p
        assert c.pythonapi.Py_GetVersion().decode().startswith(sys.version.split()[0])
        c.pythonapi.Py_IncRef.argtypes = c.pythonapi.Py_DecRef.argtypes = [c.py_object]
        c.pythonapi.Py_IncRef.restype = c.pythonapi.Py_DecRef.restype = None
        value = []
        references = sys.getrefcount(value)
        c.pythonapi.Py_IncRef(value)
        assert sys.getrefcount(value) == references + 1
        c.pythonapi.Py_DecRef(value)
        assert sys.getrefcount(value) == references

        class Complex(c.Structure):
            _fields_ = [('real', c.c_double), ('imag', c.c_double)]
        c.pythonapi.PyComplex_AsCComplex.argtypes = [c.py_object]
        c.pythonapi.PyComplex_AsCComplex.restype = Complex
        result = c.pythonapi.PyComplex_AsCComplex(3.5 - 2.25j)
        assert (result.real, result.imag) == (3.5, -2.25)
        c.pythonapi._Py_c_sum.argtypes = [Complex, Complex]
        c.pythonapi._Py_c_sum.restype = Complex
        result = c.pythonapi._Py_c_sum(Complex(1.25, 9), Complex(2.5, -2.25))
        assert (result.real, result.imag) == (3.75, 6.75)
        c.pythonapi.PyErr_Format.argtypes = [c.py_object, c.c_char_p]
        c.pythonapi.PyErr_Format.restype = c.c_void_p
        wide = (1 << 54) + 13
        try:
            c.pythonapi.PyErr_Format(ValueError, b'ffi:%d:%lld:%s',
                                    c.c_int(-7), c.c_longlong(wide), c.c_char_p(b'Blender'))
        except ValueError as error:
            assert str(error) == f'ffi:-7:{wide}:Blender'
        else:
            raise AssertionError('Python API exception was lost')
        assert process.strlen(b'recovered') == 9

        process.snprintf.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p]
        process.snprintf.restype = c.c_int
        text = c.create_string_buffer(256)
        assert process.snprintf(text, 256, b'%d|%lld|%s|%.2Lf|%d', c.c_int(-7),
                                c.c_longlong(wide), c.c_char_p(b'Blender'),
                                c.c_longdouble(1.5), c.c_int(8)) > 0
        assert text.value.decode() == f'-7|{wide}|Blender|1.50|8'
        copied = c.create_string_buffer(256)
        c.memmove(copied, text, 256)
        assert copied.raw == text.raw
        c.memset(copied, 0, 256)
        assert not any(copied.raw)

        Compare = c.CFUNCTYPE(c.c_int, c.c_void_p, c.c_void_p)
        def compare(a, b):
            x, y = c.cast(a, c.POINTER(c.c_int64))[0], c.cast(b, c.POINTER(c.c_int64))[0]
            return int(x > y) - int(x < y)
        comparator = Compare(compare)
        process.qsort.argtypes = [np.ctypeslib.ndpointer(dtype=np.int64, ndim=1,
                                 flags='C_CONTIGUOUS'), c.c_size_t, c.c_size_t, Compare]
        process.qsort.restype = None
        numeric = np.array([42, -7, wide, 1], dtype=np.int64)
        process.qsort(numeric, 4, c.sizeof(c.c_int64), comparator)
        assert numeric.tolist() == [-7, 1, 42, wide]
        try:
            process.qsort(numeric[::2], 2, c.sizeof(c.c_int64), comparator)
        except c.ArgumentError:
            pass
        else:
            raise AssertionError('Non-contiguous array was accepted')
        process.qsort(numeric, 4, c.sizeof(c.c_int64), comparator)
        assert numeric.tolist() == [-7, 1, 42, wide]
        assert list(np.ctypeslib.as_ctypes(numeric)) == numeric.tolist()
        assert np.ctypeslib.as_array(np.ctypeslib.as_ctypes(numeric)).tolist() == numeric.tolist()
        assert numeric.ctypes.data_as(c.POINTER(c.c_int64))[3] == wide
        del comparator
        gc.collect()

        # Start workers before creating callbacks, then retire and reuse table
        # slots across three real ABI signatures while the workers remain alive.
        begin, end = threading.Barrier(5), threading.Barrier(5)
        slot, results = [None], [None] * 4
        def worker(index):
            for cycle in range(60):
                begin.wait(timeout=30)
                try:
                    fn = slot[0]
                    args = [(7, 9), (wide, 4., -2), (Complex(1.25, 9),)][cycle % 3]
                    results[index] = fn(*args)
                    del fn
                except BaseException as error:
                    errors.append(repr(error))
                end.wait(timeout=30)
        workers = [threading.Thread(target=worker, args=(index,)) for index in range(4)]
        for thread in workers:
            thread.start()
        for cycle in range(60):
            mode = cycle % 3
            if mode == 0:
                slot[0] = c.CFUNCTYPE(c.c_int, c.c_int, c.c_int)(lambda a, b: a * 3 + b + 5)
                expected = 35
            elif mode == 1:
                slot[0] = c.CFUNCTYPE(c.c_int64, c.c_int64, c.c_double, c.c_int)(
                    lambda a, b, d: a + int(b) + d)
                expected = wide + 2
            else:
                slot[0] = c.CFUNCTYPE(c.c_int, Complex)(lambda item: int(item.real * 4) + int(item.imag))
                expected = 14
            begin.wait(timeout=30)
            end.wait(timeout=30)
            assert results == [expected] * 4, (cycle, results)
            slot[0] = None
            gc.collect()
        for thread in workers:
            thread.join(timeout=30)
            assert not thread.is_alive()
        callback = c.CFUNCTYPE(c.c_int, c.c_int, c.c_int)(lambda a, b: a * 3 + b + 5)
        extra = []
        workers = [threading.Thread(target=lambda: extra.append(callback(7, 9))) for _ in range(8)]
        for thread in workers:
            thread.start()
        for thread in workers:
            thread.join(timeout=30)
            assert not thread.is_alive()
        assert extra == [35] * 8
        del callback
        gc.collect()
        assert not errors, errors
        return dict(originalCtypes=True, pythonVersion=sys.version.split()[0],
                    numpyVersion=np.__version__, pointerBytes=c.sizeof(c.c_void_p),
                    longBytes=c.sizeof(c.c_long), processNamespace=True, processDataSymbol=True,
                    pythonAPI=True, pythonReferenceCounts=True, pythonAPIErrorRecovery=True,
                    complexStructABI=True, mixedVarargs=True, longDoubleFormatting=True,
                    numpyCtypesInterop=True, noncontiguousArrayRejected=True,
                    libcQsortCallback=True, memoryOperations=True, wideIntegersExact=True,
                    existingWorkers=4, reusedWorkerCallbacks=240, pythonThreadCallbacks=8,
                    signatureReuseIterations=60, unraisableErrors=errors)
    finally:
        sys.unraisablehook = original_hook


if __name__ == '__main__':
    import json
    print(json.dumps(run()))
