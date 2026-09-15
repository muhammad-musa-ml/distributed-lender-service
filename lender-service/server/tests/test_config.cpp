// Tests for environment parsing, HDFS path validation, and the shape of the SQL query.

#include "check.hpp"

#include <cstdlib>
#include <string>

#include "config.hpp"
#include "mysql_source.hpp"
#include "webhdfs.hpp"

using lender_test::check;
using lender_test::section;

void test_config() {
  section("env parsing");

  check(lender::env_or("LENDER_DEFINITELY_UNSET_XYZ", "fallback") == "fallback",
        "an unset variable falls back");
#ifndef _WIN32
  setenv("LENDER_TEST_EMPTY", "", 1);
  check(lender::env_or("LENDER_TEST_EMPTY", "fallback") == "fallback",
        "an EMPTY variable falls back too (an empty host is a misconfiguration)");
  setenv("LENDER_TEST_PORT", "6000", 1);
  check(lender::env_int_or("LENDER_TEST_PORT", 5000) == 6000, "an integer variable parses");
  setenv("LENDER_TEST_PORT", "6000x", 1);
  check(lender::env_int_or("LENDER_TEST_PORT", 5000) == 5000,
        "trailing junk is rejected rather than silently truncated to 6000");
  setenv("LENDER_TEST_PORT", "notanumber", 1);
  check(lender::env_int_or("LENDER_TEST_PORT", 5000) == 5000, "a non-number falls back");
#endif

  section("hdfs path validation");

  std::string error;
  check(lender::ValidateHdfsPath("/hdma-wi-2021.parquet", &error), "a plain absolute path is accepted");
  check(lender::ValidateHdfsPath("/partitions/55025.parquet", &error), "a nested path is accepted");
  check(!lender::ValidateHdfsPath("", &error), "the empty path is rejected");
  check(!lender::ValidateHdfsPath("relative.parquet", &error), "a relative path is rejected");
  // BlockLocations takes this straight off the wire, so these are the injection cases.
  check(!lender::ValidateHdfsPath("/a/../../etc/passwd", &error), "traversal is rejected");
  check(!lender::ValidateHdfsPath("/a.parquet?op=DELETE", &error),
        "a query-string smuggling attempt is rejected");
  check(!lender::ValidateHdfsPath("/a b.parquet", &error), "a space is rejected");
  check(!lender::ValidateHdfsPath("/a\nb", &error), "a newline is rejected");

  section("sql query shape");

  const std::string q = lender::LoansQuery();
  // The join and both bounds are specification, not preference -- pinning them here means a
  // well-meaning edit that drops the join or widens a bound fails a test instead of quietly
  // changing what gets uploaded.
  check(q.find("INNER JOIN loan_types") != std::string::npos,
        "joins loan_types (this is what supplies loan_type_name)");
  check(q.find("loan_type_name") != std::string::npos, "selects loan_type_name");
  check(q.find("loan_amount > 30000") != std::string::npos,
        "lower bound is EXCLUSIVE at 30000");
  check(q.find("loan_amount < 800000") != std::string::npos,
        "upper bound is EXCLUSIVE at 800000");
}
