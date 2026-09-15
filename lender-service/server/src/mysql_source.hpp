// Reads the joined, filtered loan rows out of MySQL and into an Arrow table.

#ifndef LENDER_MYSQL_SOURCE_HPP
#define LENDER_MYSQL_SOURCE_HPP

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <string>

namespace lender {

struct Config;

// The query DbToHdfs runs, exposed so a test can assert its shape without a database. The
// join and the bounds are the project's specification, not a tuning choice:
//   * INNER JOIN loan_types -- adds the loan_type_name column.
//   * loan_amount strictly between 30,000 and 800,000, exclusive at both ends.
// Column order matches LoanSchema() exactly; the reader below depends on that.
const char* LoansQuery();

struct ReadResult {
  std::shared_ptr<arrow::Table> table;
  std::int64_t rows = 0;
};

// Connects, runs LoansQuery(), and materialises the answer. Returns false and fills `error`
// on any failure -- connection, query, or a row that does not parse.
bool ReadLoans(const Config& cfg, ReadResult* out, std::string* error);

}  // namespace lender

#endif  // LENDER_MYSQL_SOURCE_HPP
