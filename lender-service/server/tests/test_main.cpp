#include "check.hpp"

#include <cstdio>

namespace lender_test {
int g_failures = 0;
int g_checks = 0;
}  // namespace lender_test

int main() {
  test_config();
  test_block_locations();
  test_parquet_util();
  test_cache_policy();

  // Print the POPULATION, not just the verdict. A suite that silently ran zero assertions
  // exits 0 exactly like one that ran two hundred and passed, and "0 checks" is the only
  // thing that tells them apart.
  std::printf("\n%d checks, %d failure(s)\n", lender_test::g_checks, lender_test::g_failures);
  if (lender_test::g_checks == 0) {
    std::printf("ERROR: no checks ran -- treating as failure\n");
    return 2;
  }
  return lender_test::g_failures == 0 ? 0 : 1;
}
