#include "config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace lender {

std::string env_or(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  return std::string(v);
}

int env_int_or(const char* name, int fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || *v == '\0') {
    return fallback;
  }
  try {
    std::size_t consumed = 0;
    const std::string s(v);
    const int parsed = std::stoi(s, &consumed);
    // Reject trailing junk ("5000x") rather than silently accepting the prefix, which is
    // what stoi alone would do.
    if (consumed != s.size()) {
      return fallback;
    }
    return parsed;
  } catch (const std::exception&) {
    return fallback;
  }
}

Config config_from_env() {
  Config c;
  c.listen_addr = env_or("LENDER_LISTEN_ADDR", c.listen_addr);

  c.mysql_host = env_or("MYSQL_HOST", c.mysql_host);
  c.mysql_port = static_cast<std::uint16_t>(env_int_or("MYSQL_PORT", c.mysql_port));
  c.mysql_user = env_or("MYSQL_USER", c.mysql_user);
  c.mysql_password = env_or("MYSQL_PASSWORD", c.mysql_password);
  c.mysql_db = env_or("MYSQL_DATABASE", c.mysql_db);

  c.hdfs_host = env_or("HDFS_HOST", c.hdfs_host);
  c.webhdfs_port = static_cast<std::uint16_t>(env_int_or("WEBHDFS_PORT", c.webhdfs_port));
  c.hdfs_user = env_or("HDFS_USER", c.hdfs_user);

  c.big_parquet_path = env_or("BIG_PARQUET_PATH", c.big_parquet_path);
  c.partitions_dir = env_or("PARTITIONS_DIR", c.partitions_dir);
  c.big_replication = static_cast<std::int16_t>(env_int_or("BIG_REPLICATION", c.big_replication));
  c.partition_replication =
      static_cast<std::int16_t>(env_int_or("PARTITION_REPLICATION", c.partition_replication));
  c.big_block_size = env_int_or("BIG_BLOCK_SIZE", static_cast<int>(c.big_block_size));

  c.http_timeout_s = env_int_or("HTTP_TIMEOUT_S", static_cast<int>(c.http_timeout_s));
  return c;
}

}  // namespace lender
