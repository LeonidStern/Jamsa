/*
    Jamsa Live - testutil.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The checks the Live tests share, in the style of plugin_unit_test.cpp: a
  failed check prints where it failed and the test goes on; the program's exit
  code says whether any failed. Unlike assert() they also count in Release.

*/

#ifndef JAMSA_LIVE_TESTUTIL_H
#define JAMSA_LIVE_TESTUTIL_H

#include <stdio.h>

// a build with AddressSanitizer or ThreadSanitizer: slower, also with NDEBUG
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define LIVE_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define LIVE_SANITIZED 1
#endif
#endif

static int g_failures = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fflush(stdout); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_failures++; printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); fflush(stdout); } } while (0)

// prints the summary and returns main()'s exit code
static int test_summary(const char *name)
{
  printf("%s: %d checks, %d failed\n", name, g_checks, g_failures);
  return g_failures ? 1 : 0;
}

#endif
