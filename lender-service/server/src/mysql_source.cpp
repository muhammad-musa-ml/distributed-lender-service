#include "mysql_source.hpp"

#include <mysql/mysql.h>

#include <arrow/api.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>

#include "config.hpp"
#include "parquet_util.hpp"

namespace lender {
namespace {

// RAII for the two C handles, so the dozen early returns below cannot leak a connection or
// a result set.
class MysqlConn {
 public:
  MysqlConn() : h_(mysql_init(nullptr)) {}
  ~MysqlConn() {
    if (h_ != nullptr) {
      mysql_close(h_);
    }
  }
  MysqlConn(const MysqlConn&) = delete;
  MysqlConn& operator=(const MysqlConn&) = delete;
  MYSQL* get() const { return h_; }

 private:
  MYSQL* h_;
};

class MysqlRes {
 public:
  explicit MysqlRes(MYSQL_RES* r) : r_(r) {}
  ~MysqlRes() {
    if (r_ != nullptr) {
      mysql_free_result(r_);
    }
  }
  MysqlRes(const MysqlRes&) = delete;
  MysqlRes& operator=(const MysqlRes&) = delete;
  MYSQL_RES* get() const { return r_; }

 private:
  MYSQL_RES* r_;
};

// Strict integer parse: rejects trailing junk rather than silently taking a prefix.
bool ParseInt32(const char* s, std::int32_t* out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return false;
  }
  if (v < -2147483648L || v > 2147483647L) {
    return false;
  }
  *out = static_cast<std::int32_t>(v);
  return true;
}

bool ParseDouble(const char* s, double* out) {
  if (s == nullptr || *s == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0') {
    return false;
  }
  *out = v;
  return true;
}

}  // namespace

const char* LoansQuery() {
  return "SELECT l.county_code, l.loan_type, t.loan_type_name, l.loan_amount, "
         "l.income, l.interest_rate "
         "FROM loans AS l "
         "INNER JOIN loan_types AS t ON l.loan_type = t.loan_type_id "
         "WHERE l.loan_amount > 30000 AND l.loan_amount < 800000";
}

bool ReadLoans(const Config& cfg, ReadResult* out, std::string* error) {
  MysqlConn conn;
  if (conn.get() == nullptr) {
    *error = "mysql_init failed (out of memory)";
    return false;
  }

  if (mysql_real_connect(conn.get(), cfg.mysql_host.c_str(), cfg.mysql_user.c_str(),
                         cfg.mysql_password.c_str(), cfg.mysql_db.c_str(),
                         static_cast<unsigned int>(cfg.mysql_port), nullptr,
                         0) == nullptr) {
    // Deliberately reports host/port/db but never the password.
    *error = "mysql connect " + cfg.mysql_host + ":" + std::to_string(cfg.mysql_port) + "/" +
             cfg.mysql_db + ": " + mysql_error(conn.get());
    return false;
  }

  if (mysql_query(conn.get(), LoansQuery()) != 0) {
    *error = std::string("mysql query: ") + mysql_error(conn.get());
    return false;
  }

  // use_result streams rows as they arrive rather than buffering the whole set in the
  // client first. store_result would be one line shorter and would hold the full result in
  // client memory alongside the Arrow builders that are about to hold it again.
  MysqlRes res(mysql_use_result(conn.get()));
  if (res.get() == nullptr) {
    *error = std::string("mysql_use_result: ") + mysql_error(conn.get());
    return false;
  }

  const unsigned int expected_fields = 6;  // must match LoanSchema()
  const unsigned int nfields = mysql_num_fields(res.get());
  if (nfields != expected_fields) {
    *error = "query returned " + std::to_string(nfields) + " columns, expected " +
             std::to_string(expected_fields);
    return false;
  }

  arrow::Int32Builder county_code;
  arrow::Int32Builder loan_type;
  arrow::StringBuilder loan_type_name;
  arrow::Int32Builder loan_amount;
  arrow::Int32Builder income;
  arrow::DoubleBuilder interest_rate;

  std::int64_t rows = 0;
  MYSQL_ROW row = nullptr;
  while ((row = mysql_fetch_row(res.get())) != nullptr) {
    std::int32_t i32 = 0;
    double f64 = 0.0;
    arrow::Status st;

    // county_code, loan_type, loan_amount are required by the query's own WHERE clause and
    // join; a NULL there means the data does not match the schema this server was written
    // against, and failing loudly beats writing a Parquet file full of nulls.
    if (!ParseInt32(row[0], &i32)) {
      *error = "row " + std::to_string(rows) + ": bad county_code";
      return false;
    }
    st = county_code.Append(i32);
    if (!st.ok()) { *error = "append county_code: " + st.ToString(); return false; }

    if (!ParseInt32(row[1], &i32)) {
      *error = "row " + std::to_string(rows) + ": bad loan_type";
      return false;
    }
    st = loan_type.Append(i32);
    if (!st.ok()) { *error = "append loan_type: " + st.ToString(); return false; }

    st = (row[2] == nullptr) ? loan_type_name.AppendNull()
                             : loan_type_name.Append(row[2]);
    if (!st.ok()) { *error = "append loan_type_name: " + st.ToString(); return false; }

    if (!ParseInt32(row[3], &i32)) {
      *error = "row " + std::to_string(rows) + ": bad loan_amount";
      return false;
    }
    st = loan_amount.Append(i32);
    if (!st.ok()) { *error = "append loan_amount: " + st.ToString(); return false; }

    // income and interest_rate are genuinely optional in the source data.
    if (row[4] == nullptr || !ParseInt32(row[4], &i32)) {
      st = income.AppendNull();
    } else {
      st = income.Append(i32);
    }
    if (!st.ok()) { *error = "append income: " + st.ToString(); return false; }

    if (row[5] == nullptr || !ParseDouble(row[5], &f64)) {
      st = interest_rate.AppendNull();
    } else {
      st = interest_rate.Append(f64);
    }
    if (!st.ok()) { *error = "append interest_rate: " + st.ToString(); return false; }

    ++rows;
  }

  // With use_result, a mid-stream network failure ends the loop exactly like a clean
  // finish. Without this check a truncated read would be uploaded to HDFS as if complete.
  if (mysql_errno(conn.get()) != 0) {
    *error = std::string("mysql row stream ended early: ") + mysql_error(conn.get());
    return false;
  }

  std::shared_ptr<arrow::Array> a_county, a_type, a_name, a_amount, a_income, a_rate;
  arrow::Status st;
  st = county_code.Finish(&a_county);
  if (!st.ok()) { *error = "finish county_code: " + st.ToString(); return false; }
  st = loan_type.Finish(&a_type);
  if (!st.ok()) { *error = "finish loan_type: " + st.ToString(); return false; }
  st = loan_type_name.Finish(&a_name);
  if (!st.ok()) { *error = "finish loan_type_name: " + st.ToString(); return false; }
  st = loan_amount.Finish(&a_amount);
  if (!st.ok()) { *error = "finish loan_amount: " + st.ToString(); return false; }
  st = income.Finish(&a_income);
  if (!st.ok()) { *error = "finish income: " + st.ToString(); return false; }
  st = interest_rate.Finish(&a_rate);
  if (!st.ok()) { *error = "finish interest_rate: " + st.ToString(); return false; }

  out->table = arrow::Table::Make(LoanSchema(),
                                  {a_county, a_type, a_name, a_amount, a_income, a_rate});
  out->rows = rows;
  return true;
}

}  // namespace lender
