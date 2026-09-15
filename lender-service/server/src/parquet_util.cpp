#include "parquet_util.hpp"

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lender {
namespace {

// WHY THERE IS NO arrow::compute HERE.
//
// The obvious way to write FilterByCounty and AverageLoanAmount is CallFunction("equal"),
// CallFunction("filter"), CallFunction("sum"). MEASURED in this image: those three return
// `Key error: No function registered with name: equal`. Arrow 25 ships its kernels in a
// separate libarrow-compute, which libarrow-dev does not pull in -- libarrow.so exports
// GetFunctionRegistry but zero kernel-registration symbols, so the registry is empty.
//
// That left a choice: add libarrow-compute-dev, or write the two operations. This file
// writes them, because the operations are an int32 equality filter and an int64 sum -- no
// external format to misinterpret, no spec to get subtly wrong, and both pinned by tests
// that assert the exact semantics (floor, overflow, empty input). The alternative would put
// a large kernel library into the runtime image to do integer comparison and addition.
//
// The contrast with the JSON parser is deliberate and is the rule being applied: a real
// library earns its place where the problem is interpreting SOMEONE ELSE'S format, and does
// not where the problem is adding up numbers we produced.

// Appends the rows of `src` selected by `keep` onto `builder`. One overload per type that
// LoanSchema() actually uses; an unexpected type is a loud failure rather than a silent drop.
// `append_value` has signature (BuilderT*, const ArrayT&, int64_t) -> arrow::Status and is
// called only for non-null selected rows; nulls are handled here so every column treats
// them identically.
template <typename ArrayT, typename BuilderT, typename AppendFn>
bool AppendSelected(const arrow::ChunkedArray& src,
                    const std::vector<bool>& keep,
                    BuilderT* builder,
                    AppendFn append_value,
                    std::string* error) {
  std::int64_t global = 0;
  for (const auto& chunk : src.chunks()) {
    const auto* typed = dynamic_cast<const ArrayT*>(chunk.get());
    if (typed == nullptr) {
      *error = "column chunk has an unexpected physical type";
      return false;
    }
    for (std::int64_t i = 0; i < typed->length(); ++i, ++global) {
      if (!keep[static_cast<std::size_t>(global)]) {
        continue;
      }
      arrow::Status st = typed->IsNull(i) ? builder->AppendNull()
                                          : append_value(builder, *typed, i);
      if (!st.ok()) {
        *error = "append: " + st.ToString();
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::shared_ptr<arrow::Schema> LoanSchema() {
  return arrow::schema({
      arrow::field("county_code", arrow::int32()),
      arrow::field("loan_type", arrow::int32()),
      arrow::field("loan_type_name", arrow::utf8()),
      arrow::field("loan_amount", arrow::int32()),
      arrow::field("income", arrow::int32()),
      arrow::field("interest_rate", arrow::float64()),
  });
}

bool TableToParquetBytes(const std::shared_ptr<arrow::Table>& table,
                         std::string* out,
                         std::string* error) {
  if (table == nullptr) {
    *error = "TableToParquetBytes: null table";
    return false;
  }
  auto sink_result = arrow::io::BufferOutputStream::Create();
  if (!sink_result.ok()) {
    *error = "BufferOutputStream::Create: " + sink_result.status().ToString();
    return false;
  }
  std::shared_ptr<arrow::io::BufferOutputStream> sink = *sink_result;

  // chunk_size is the Parquet ROW-GROUP size, independent of the HDFS block size set at
  // upload. 64Ki rows keeps row groups comfortably readable while still letting the 1 MB
  // HDFS blocks the project asks for split the file across several DataNodes.
  const arrow::Status st =
      parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), sink, 65536);
  if (!st.ok()) {
    *error = "parquet::arrow::WriteTable: " + st.ToString();
    return false;
  }

  auto buffer_result = sink->Finish();
  if (!buffer_result.ok()) {
    *error = "BufferOutputStream::Finish: " + buffer_result.status().ToString();
    return false;
  }
  const std::shared_ptr<arrow::Buffer> buffer = *buffer_result;
  out->assign(reinterpret_cast<const char*>(buffer->data()),
              static_cast<std::size_t>(buffer->size()));
  return true;
}

bool ParquetBytesToTable(const std::string& bytes,
                         std::shared_ptr<arrow::Table>* out,
                         std::string* error) {
  if (bytes.empty()) {
    *error = "ParquetBytesToTable: empty input";
    return false;
  }
  auto buffer = std::make_shared<arrow::Buffer>(
      reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int64_t>(bytes.size()));
  auto reader_source = std::make_shared<arrow::io::BufferReader>(buffer);

  // Arrow 25 returns Result<> from both of these; the out-parameter overloads are
  // deprecated (ReadTable) or removed (OpenFile).
  auto reader_result = parquet::arrow::OpenFile(reader_source, arrow::default_memory_pool());
  if (!reader_result.ok()) {
    *error = "parquet::arrow::OpenFile: " + reader_result.status().ToString();
    return false;
  }
  std::unique_ptr<parquet::arrow::FileReader> reader = std::move(*reader_result);

  auto table_result = reader->ReadTable();
  if (!table_result.ok()) {
    *error = "FileReader::ReadTable: " + table_result.status().ToString();
    return false;
  }
  *out = *table_result;
  return true;
}

bool FilterByCounty(const std::shared_ptr<arrow::Table>& table,
                    std::int32_t county_code,
                    std::shared_ptr<arrow::Table>* out,
                    std::string* error) {
  if (table == nullptr) {
    *error = "FilterByCounty: null table";
    return false;
  }
  const std::shared_ptr<arrow::ChunkedArray> key = table->GetColumnByName("county_code");
  if (key == nullptr) {
    *error = "FilterByCounty: table has no 'county_code' column";
    return false;
  }

  // A row mask rather than an index list: it makes the per-column pass a straight walk with
  // no offset arithmetic between chunk-local and table-global positions, which is where a
  // hand-written selection usually goes wrong.
  const std::int64_t n_rows = table->num_rows();
  std::vector<bool> keep(static_cast<std::size_t>(n_rows), false);
  std::int64_t matched = 0;
  {
    std::int64_t global = 0;
    for (const auto& chunk : key->chunks()) {
      const auto* typed = dynamic_cast<const arrow::Int32Array*>(chunk.get());
      if (typed == nullptr) {
        *error = "FilterByCounty: 'county_code' is not int32";
        return false;
      }
      for (std::int64_t i = 0; i < typed->length(); ++i, ++global) {
        // A null county_code matches nothing, which is the right reading: an unknown county
        // is not the requested county.
        if (!typed->IsNull(i) && typed->Value(i) == county_code) {
          keep[static_cast<std::size_t>(global)] = true;
          ++matched;
        }
      }
    }
    if (global != n_rows) {
      *error = "FilterByCounty: chunk lengths do not sum to the table row count";
      return false;
    }
  }
  (void)matched;

  const std::shared_ptr<arrow::Schema> schema = table->schema();
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(static_cast<std::size_t>(schema->num_fields()));

  for (int f = 0; f < schema->num_fields(); ++f) {
    const std::shared_ptr<arrow::Field> field = schema->field(f);
    const std::shared_ptr<arrow::ChunkedArray> col = table->column(f);
    std::shared_ptr<arrow::Array> built;
    arrow::Status st;

    if (field->type()->Equals(arrow::int32())) {
      arrow::Int32Builder b;
      if (!AppendSelected<arrow::Int32Array>(
              *col, keep, &b,
              [](arrow::Int32Builder* dst, const arrow::Int32Array& a, std::int64_t i) {
                return dst->Append(a.Value(i));
              },
              error)) {
        return false;
      }
      st = b.Finish(&built);
    } else if (field->type()->Equals(arrow::utf8())) {
      arrow::StringBuilder b;
      if (!AppendSelected<arrow::StringArray>(
              *col, keep, &b,
              [](arrow::StringBuilder* dst, const arrow::StringArray& a, std::int64_t i) {
                return dst->Append(a.GetView(i));
              },
              error)) {
        return false;
      }
      st = b.Finish(&built);
    } else if (field->type()->Equals(arrow::float64())) {
      arrow::DoubleBuilder b;
      if (!AppendSelected<arrow::DoubleArray>(
              *col, keep, &b,
              [](arrow::DoubleBuilder* dst, const arrow::DoubleArray& a, std::int64_t i) {
                return dst->Append(a.Value(i));
              },
              error)) {
        return false;
      }
      st = b.Finish(&built);
    } else {
      // Reached only if LoanSchema() gains a type this function was never taught. Failing
      // here beats returning a table that silently dropped a column.
      *error = "FilterByCounty: unsupported column type '" + field->type()->ToString() +
               "' for field '" + field->name() + "'";
      return false;
    }

    if (!st.ok()) {
      *error = "finish column '" + field->name() + "': " + st.ToString();
      return false;
    }
    columns.push_back(built);
  }

  *out = arrow::Table::Make(schema, columns);
  return true;
}

std::optional<std::int32_t> AverageLoanAmount(const std::shared_ptr<arrow::Table>& table,
                                              std::string* error) {
  if (table == nullptr) {
    *error = "AverageLoanAmount: null table";
    return std::nullopt;
  }
  const std::shared_ptr<arrow::ChunkedArray> col = table->GetColumnByName("loan_amount");
  if (col == nullptr) {
    *error = "AverageLoanAmount: table has no 'loan_amount' column";
    return std::nullopt;
  }

  // int64 accumulator, deliberately. The project's filtered set is ~426,716 rows bounded
  // above by 800,000, so the worst-case total is ~3.4e11 -- comfortably past int32's 2.1e9
  // and trivial for int64. A 32-bit accumulator here would wrap into a negative average on
  // any realistically sized county.
  std::int64_t total = 0;
  std::int64_t n = 0;

  for (const auto& chunk : col->chunks()) {
    const auto* typed = dynamic_cast<const arrow::Int32Array*>(chunk.get());
    if (typed == nullptr) {
      *error = "AverageLoanAmount: 'loan_amount' is not int32";
      return std::nullopt;
    }
    for (std::int64_t i = 0; i < typed->length(); ++i) {
      if (typed->IsNull(i)) {
        continue;
      }
      total += static_cast<std::int64_t>(typed->Value(i));
      ++n;
    }
  }

  if (n == 0) {
    // Not an error: the caller decides what an empty county means.
    return std::nullopt;
  }
  if (total < 0) {
    *error = "AverageLoanAmount: negative total, refusing to floor a negative mean";
    return std::nullopt;
  }
  // Integer division of a non-negative total IS the floor -- no double, so no chance of a
  // true mean of 250000 arriving as 249999.99999999997 and flooring to one less.
  return static_cast<std::int32_t>(total / n);
}

}  // namespace lender
