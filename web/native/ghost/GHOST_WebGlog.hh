/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

/* Bundled glog configuration for Emscripten's POSIX libc and real pthreads.
 * Native stack unwinding, process syscalls and syslog are unavailable. */
#define GOOGLE_NAMESPACE google
#define STL_NAMESPACE std
#define _START_GOOGLE_NAMESPACE_ namespace google {
#define _END_GOOGLE_NAMESPACE_ }
#ifndef GOOGLE_GLOG_DLL_DECL
#  define GOOGLE_GLOG_DLL_DECL
#endif
#define SIZEOF_VOID_P __SIZEOF_POINTER__
#define TEST_SRC_DIR "."
#define HAVE_SNPRINTF 1
#define HAVE_FCNTL 1
#define HAVE_INTTYPES_H 1
#define HAVE_LIB_GFLAGS 1
#define HAVE_MEMORY_H 1
#define HAVE_NAMESPACES 1
#define HAVE_PREAD 1
#define HAVE_PTHREAD 1
#define HAVE_PWRITE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1
#define HAVE_USING_OPERATOR 1
#define HAVE___ATTRIBUTE__ 1
#define HAVE___BUILTIN_EXPECT 1
#define HAVE___SYNC_VAL_COMPARE_AND_SWAP 1
