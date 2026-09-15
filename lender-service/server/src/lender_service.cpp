#include "lender_service.hpp"

#include <arrow/api.h>

#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "block_locations.hpp"
#include "cache_policy.hpp"
#include "mysql_source.hpp"
#include "parquet_util.hpp"

namespace lender {

LenderServiceImpl::LenderServiceImpl(Config cfg, HdfsClient* hdfs)
    : cfg_(std::move(cfg)), hdfs_(hdfs) {}

std::mutex& LenderServiceImpl::LockForCounty(std::int32_t county_code) {
  std::lock_guard<std::mutex> guard(stripe_table_mu_);
  auto it = county_mu_.find(county_code);
  if (it == county_mu_.end()) {
    it = county_mu_.emplace(county_code, std::make_unique<std::mutex>()).first;
  }
  // Safe to hand out a reference: the map holds unique_ptrs, so rehashing/rebalancing never
  // moves the mutex itself, and entries are never erased for the life of the server.
  return *it->second;
}

grpc::Status LenderServiceImpl::DbToHdfs(grpc::ServerContext*,
                                         const Empty*,
                                         StatusString* resp) {
  std::lock_guard<std::mutex> guard(big_file_mu_);

  ReadResult read;
  std::string error;
  if (!ReadLoans(cfg_, &read, &error)) {
    resp->set_status("error: " + error);
    return grpc::Status::OK;
  }

  std::string bytes;
  if (!TableToParquetBytes(read.table, &bytes, &error)) {
    resp->set_status("error: " + error);
    return grpc::Status::OK;
  }

  if (!hdfs_->Write(cfg_.big_parquet_path, bytes, cfg_.big_replication, cfg_.big_block_size,
                    &error)) {
    resp->set_status("error: " + error);
    return grpc::Status::OK;
  }

  resp->set_status("wrote " + std::to_string(read.rows) + " rows (" +
                   std::to_string(bytes.size()) + " bytes) to " + cfg_.big_parquet_path +
                   " at replication " + std::to_string(cfg_.big_replication) +
                   ", block size " + std::to_string(cfg_.big_block_size));
  return grpc::Status::OK;
}

grpc::Status LenderServiceImpl::BlockLocations(grpc::ServerContext*,
                                               const BlockLocationsReq* req,
                                               BlockLocationsResp* resp) {
  // client.py's -f defaults to "", and the file everyone actually wants to inspect is the
  // big one, so an empty path means that rather than an error.
  std::string path = req->path();
  if (path.empty()) {
    path = cfg_.big_parquet_path;
  }

  std::string json;
  std::string error;
  if (!hdfs_->BlockLocationsJson(path, &json, &error)) {
    resp->set_error(error);
    return grpc::Status::OK;
  }

  BlockCounts counts;
  if (!ParseBlockLocations(json, &counts, &error)) {
    resp->set_error(error);
    return grpc::Status::OK;
  }

  auto& entries = *resp->mutable_block_entries();
  for (const auto& [host, n] : counts) {
    entries[host] = n;
  }
  return grpc::Status::OK;
}

grpc::Status LenderServiceImpl::CalcAvgLoan(grpc::ServerContext*,
                                            const CalcAvgLoanReq* req,
                                            CalcAvgLoanResp* resp) {
  const std::int32_t county = req->county_code();
  const std::string partition = PartitionPath(cfg_.partitions_dir, county);

  std::lock_guard<std::mutex> guard(LockForCounty(county));

  std::string error;

  // --- Observation 1: what does the NameNode say about the cached partition? -------------
  const Tri exists = hdfs_->Exists(partition, &error);

  // --- Observation 2: can we actually read and decode it? -------------------------------
  // Both halves matter. Bytes that arrive but do not decode are a corrupt cache, which the
  // specification treats exactly like a lost one.
  bool cache_usable = false;
  std::shared_ptr<arrow::Table> cached;
  if (ShouldAttemptCacheRead(exists)) {
    std::string bytes;
    std::string read_error;
    if (hdfs_->Read(partition, &bytes, &read_error)) {
      std::string decode_error;
      if (ParquetBytesToTable(bytes, &cached, &decode_error)) {
        cache_usable = true;
      }
    }
  }

  const Source source = DecideSource(exists, cache_usable);

  std::shared_ptr<arrow::Table> county_rows;
  if (source == Source::kReuse) {
    county_rows = cached;
  } else {
    // Rebuild from the big file. This is the path that survives a DataNode failure: the big
    // file is replicated twice precisely so it is still readable when a 1x partition is not.
    std::string bytes;
    if (!hdfs_->Read(cfg_.big_parquet_path, &bytes, &error)) {
      resp->set_error("cannot read " + cfg_.big_parquet_path + " (has DbToHdfs run?): " +
                      error);
      return grpc::Status::OK;
    }
    std::shared_ptr<arrow::Table> big;
    if (!ParquetBytesToTable(bytes, &big, &error)) {
      resp->set_error("cannot decode " + cfg_.big_parquet_path + ": " + error);
      return grpc::Status::OK;
    }
    if (!FilterByCounty(big, county, &county_rows, &error)) {
      resp->set_error(error);
      return grpc::Status::OK;
    }

    // Persist the partition for next time, at 1x replication per the storage policy.
    //
    // A failure to write is NOT fatal: we already hold the correct answer, and refusing to
    // return it because the cache could not be updated would turn a degraded cluster into a
    // broken RPC. The write is an optimisation for later calls.
    //
    // It is logged to the server's stderr rather than into resp.error, and that distinction
    // is load-bearing rather than stylistic: the reference client (../../client.py) branches
    // on `if resp.error:` and prints the message INSTEAD of the answer. Putting a warning
    // there would make a successful call look like a failed one and hide the average the
    // caller asked for. resp.error means "this RPC did not answer your question"; anything
    // else belongs in the log.
    std::string out_bytes;
    std::string write_error;
    if (TableToParquetBytes(county_rows, &out_bytes, &write_error)) {
      if (!hdfs_->Write(partition, out_bytes, cfg_.partition_replication,
                        cfg_.big_block_size, &write_error)) {
        std::cerr << "warn: computed the average for county " << county
                  << " but could not cache " << partition << ": " << write_error << "\n";
      }
    } else {
      std::cerr << "warn: computed the average for county " << county
                << " but could not encode " << partition << ": " << write_error << "\n";
    }
  }

  const std::optional<std::int32_t> avg = AverageLoanAmount(county_rows, &error);
  if (!avg.has_value()) {
    if (!error.empty()) {
      resp->set_error(error);
      return grpc::Status::OK;
    }
    // No rows for this county. Reporting 0 would be indistinguishable from a county whose
    // loans really do average zero, so say what happened instead.
    resp->set_error("no loans for county_code " + std::to_string(county));
    resp->set_source(SourceName(source));
    return grpc::Status::OK;
  }

  resp->set_avg_loan(*avg);
  resp->set_source(SourceName(source));
  return grpc::Status::OK;
}

}  // namespace lender
