// Tests for the Parquet round trip, the county filter, and the floored average.

#include "check.hpp"

#include <arrow/api.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "parquet_util.hpp"

using lender_test::check;
using lender_test::check_detail;
using lender_test::section;

namespace {

// Builds a table against the real LoanSchema() so a schema change breaks the tests rather
// than silently diverging from what the server writes.
std::shared_ptr<arrow::Table> MakeTable(const std::vector<std::int32_t>& county,
                                        const std::vector<std::int32_t>& amount) {
  arrow::Int32Builder b_county, b_type, b_amount, b_income;
  arrow::StringBuilder b_name;
  arrow::DoubleBuilder b_rate;
  for (std::size_t i = 0; i < county.size(); ++i) {
    (void)b_county.Append(county[i]);
    (void)b_type.Append(1);
    (void)b_name.Append("Conventional");
    (void)b_amount.Append(amount[i]);
    (void)b_income.Append(90);
    (void)b_rate.Append(3.25);
  }
  std::shared_ptr<arrow::Array> a_county, a_type, a_name, a_amount, a_income, a_rate;
  (void)b_county.Finish(&a_county);
  (void)b_type.Finish(&a_type);
  (void)b_name.Finish(&a_name);
  (void)b_amount.Finish(&a_amount);
  (void)b_income.Finish(&a_income);
  (void)b_rate.Finish(&a_rate);
  return arrow::Table::Make(lender::LoanSchema(),
                            {a_county, a_type, a_name, a_amount, a_income, a_rate});
}

}  // namespace

void test_parquet_util() {
  section("parquet round trip");

  {
    auto table = MakeTable({55025, 55025, 55001}, {100000, 200000, 300000});
    std::string bytes;
    std::string error;
    const bool wrote = lender::TableToParquetBytes(table, &bytes, &error);
    check(wrote, "encodes a table to Parquet bytes");
    check(bytes.size() > 4 && bytes.compare(0, 4, "PAR1") == 0,
          "output carries the Parquet magic 'PAR1'");

    std::shared_ptr<arrow::Table> back;
    const bool read = lender::ParquetBytesToTable(bytes, &back, &error);
    check(read, "decodes those bytes back to a table");
    check(back != nullptr && back->num_rows() == 3, "row count survives the round trip");
    check(back != nullptr && back->schema()->Equals(*lender::LoanSchema()),
          "schema survives the round trip unchanged");
  }

  {
    std::shared_ptr<arrow::Table> out;
    std::string error;
    check(!lender::ParquetBytesToTable("", &out, &error), "empty input is rejected");
    check(!lender::ParquetBytesToTable("not parquet at all", &out, &error),
          "garbage input is rejected rather than decoded into an empty table");
  }

  section("county filter");
  {
    auto table = MakeTable({55025, 55001, 55025, 55003}, {10, 20, 30, 40});
    std::shared_ptr<arrow::Table> filtered;
    std::string error;
    check_detail(lender::FilterByCounty(table, 55025, &filtered, &error), "filter succeeds",
                 error);
    check(filtered != nullptr && filtered->num_rows() == 2, "keeps exactly the two matches");

    std::shared_ptr<arrow::Table> none;
    check(lender::FilterByCounty(table, 99999, &none, &error),
          "filtering to a county with no rows is not an error");
    check(none != nullptr && none->num_rows() == 0, "and yields zero rows");
  }

  section("floored average");
  {
    std::string error;
    // 10 + 20 + 31 = 61; 61 / 3 = 20.333..., so the contract's answer is 20.
    auto table = MakeTable({1, 1, 1}, {10, 20, 31});
    auto avg = lender::AverageLoanAmount(table, &error);
    check_detail(avg.has_value() && *avg == 20, "20.33 rounds DOWN to 20", error);

    // THE DISCRIMINATING CASE, and the reason the one above is not enough.
    //
    // A mean of 20.33 floors to 20 AND rounds-to-nearest to 20, so a test built on it
    // passes just as happily against a round-half-up implementation -- which is exactly
    // what a mutation run proved: replacing the integer division with
    // `(double)total/n + 0.5` left every assertion in this section green.
    //
    // Only a fractional part >= .5 separates the two. 10 + 20 + 32 = 62; 62/3 = 20.667,
    // which floors to 20 and rounds to 21.
    auto down = MakeTable({1, 1, 1}, {10, 20, 32});
    avg = lender::AverageLoanAmount(down, &error);
    check_detail(avg.has_value() && *avg == 20,
                 "20.67 rounds DOWN to 20, NOT to nearest (21)", error);

    // The exact .5 boundary, where round-half-up and floor disagree by one.
    auto half = MakeTable({1, 1}, {1, 2});
    avg = lender::AverageLoanAmount(half, &error);
    check_detail(avg.has_value() && *avg == 1, "an exact .5 mean floors to 1, not 2", error);

    // The case a floating-point mean gets wrong: 3 values averaging exactly 250000 can come
    // back as 249999.99999999997 through a double, which floors to one less.
    auto exact = MakeTable({1, 1, 1}, {250000, 250000, 250000});
    avg = lender::AverageLoanAmount(exact, &error);
    check(avg.has_value() && *avg == 250000, "an exact mean is not dragged below by rounding");

    auto one = MakeTable({1}, {799999});
    avg = lender::AverageLoanAmount(one, &error);
    check(avg.has_value() && *avg == 799999, "a single row averages to itself");

    auto empty = MakeTable({}, {});
    error.clear();
    avg = lender::AverageLoanAmount(empty, &error);
    check(!avg.has_value(), "no rows yields no value");
    check(error.empty(), "and that is reported as absence, not as an error");
  }

  {
    // 426,716 rows near the upper bound sum to ~3.4e11, which overflows int32. If the sum
    // were accumulated in 32 bits this would come back negative or wrapped.
    std::vector<std::int32_t> county(200000, 55025);
    std::vector<std::int32_t> amount(200000, 799999);
    auto big = MakeTable(county, amount);
    std::string error;
    auto avg = lender::AverageLoanAmount(big, &error);
    check(avg.has_value() && *avg == 799999,
          "200k rows near the max do not overflow the accumulator");
  }
}
