// In-memory Parquet encode/decode plus the two table operations CalcAvgLoan needs.
//
// Everything here works on buffers rather than files. The server never touches local disk:
// DbToHdfs serialises a table into memory and PUTs it, CalcAvgLoan GETs bytes and decodes
// them. That keeps the container stateless and means these functions are testable without
// a filesystem, a cluster, or a temp directory.

#ifndef LENDER_PARQUET_UTIL_HPP
#define LENDER_PARQUET_UTIL_HPP

#include <arrow/api.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace lender {

// The one declaration of the Parquet layout, shared by the MySQL reader that builds tables
// and the decoder that reads them back. A single source for the schema is what stops the
// writer and the reader from drifting into disagreeing about column order or type.
//
// Column order matches the SELECT in mysql_source.cpp.
std::shared_ptr<arrow::Schema> LoanSchema();

// Serialise a table to Parquet bytes. `error` is filled on failure.
bool TableToParquetBytes(const std::shared_ptr<arrow::Table>& table,
                         std::string* out,
                         std::string* error);

// Decode Parquet bytes back into a table.
bool ParquetBytesToTable(const std::string& bytes,
                         std::shared_ptr<arrow::Table>* out,
                         std::string* error);

// Rows whose county_code equals `county_code`. An empty result is a valid table with zero
// rows, not an error -- a county with no loans is a real answer.
bool FilterByCounty(const std::shared_ptr<arrow::Table>& table,
                    std::int32_t county_code,
                    std::shared_ptr<arrow::Table>* out,
                    std::string* error);

// Mean loan_amount, rounded DOWN, as the RPC's int32.
//
// Deliberately computed as an integer sum divided by an integer count rather than as a
// floating-point mean that is then floored. Loan amounts are positive integers, so integer
// division IS the floor, and this cannot produce the classic off-by-one where a true mean
// of 250000 arrives as 249999.99999999997 and floors to 249999.
//
// Returns nullopt when there are no non-null rows: the caller must distinguish "the average
// is zero" from "there is nothing to average".
std::optional<std::int32_t> AverageLoanAmount(const std::shared_ptr<arrow::Table>& table,
                                              std::string* error);

}  // namespace lender

#endif  // LENDER_PARQUET_UTIL_HPP
