// Runtime configuration, read from the environment.
//
// Everything that varies by environment lives here from the first commit rather than being
// retrofitted later: the compose topology supplies hostnames (`mysql`, `boss`) that differ
// from what a laptop run would use, and the MySQL password is a credential that must never
// be a literal in a source file that is about to become a public repository.

#ifndef LENDER_CONFIG_HPP
#define LENDER_CONFIG_HPP

#include <cstdint>
#include <string>

namespace lender {

struct Config {
  // gRPC
  std::string listen_addr = "0.0.0.0:5000";

  // MySQL
  std::string mysql_host = "mysql";
  std::uint16_t mysql_port = 3306;
  std::string mysql_user = "root";
  std::string mysql_password = "abc";
  std::string mysql_db = "CS544";

  // HDFS (WebHDFS REST endpoint on the NameNode)
  std::string hdfs_host = "boss";
  std::uint16_t webhdfs_port = 9870;
  std::string hdfs_user = "root";

  // Paths and storage policy, from the project specification.
  std::string big_parquet_path = "/hdma-wi-2021.parquet";
  std::string partitions_dir = "/partitions";
  std::int16_t big_replication = 2;        // survives one DataNode loss
  std::int16_t partition_replication = 1;  // deliberately fragile; see cache_policy.hpp
  std::int64_t big_block_size = 1024 * 1024;

  // Seconds. A read of the big Parquet file is the slow path and must not be cut off, but
  // a hung DataNode must not wedge an RPC forever either.
  long http_timeout_s = 120;
};

// Reads the process environment, falling back to the defaults above for anything unset.
Config config_from_env();

// Exposed for testing: resolves one variable with a default. Returns `fallback` when the
// variable is unset OR set to the empty string -- an empty MYSQL_HOST is a misconfiguration,
// not a request to connect to "".
std::string env_or(const char* name, const std::string& fallback);
int env_int_or(const char* name, int fallback);

}  // namespace lender

#endif  // LENDER_CONFIG_HPP
