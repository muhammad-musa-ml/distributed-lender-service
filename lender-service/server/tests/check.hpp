// Minimal test harness, deliberately the same shape as ../lowlat/src/test_ring.cpp:
// a check() that prints one line per assertion and increments a shared failure count.
//
// No gtest. It would be a second dependency in the toolchain image, and what these tests
// need -- run everything, report every failure, exit non-zero if any failed -- is about
// twenty lines. The bar that matters is that a failure is loud and names itself, which this
// clears.

#ifndef LENDER_TESTS_CHECK_HPP
#define LENDER_TESTS_CHECK_HPP

#include <cstdio>
#include <string>

namespace lender_test {

extern int g_failures;
extern int g_checks;

inline void check(bool cond, const char* what) {
  ++g_checks;
  if (cond) {
    std::printf("  ok    %s\n", what);
  } else {
    std::printf("  FAIL  %s\n", what);
    ++g_failures;
  }
}

// Same, but prints the operation's own error text on failure. Use this wherever the thing
// under test produces a diagnostic: a failing assertion that cannot say WHY costs a whole
// debugging round trip to reproduce something the code already knew.
inline void check_detail(bool cond, const char* what, const std::string& detail) {
  ++g_checks;
  if (cond) {
    std::printf("  ok    %s\n", what);
  } else {
    std::printf("  FAIL  %s\n          -> %s\n", what, detail.c_str());
    ++g_failures;
  }
}

inline void section(const char* name) { std::printf("[%s]\n", name); }

}  // namespace lender_test

// Each test file exposes one entry point; test_main.cpp calls them all.
void test_block_locations();
void test_parquet_util();
void test_cache_policy();
void test_config();

#endif  // LENDER_TESTS_CHECK_HPP
