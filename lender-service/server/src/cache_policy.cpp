#include "cache_policy.hpp"

#include <string>

namespace lender {

const char* SourceName(Source s) {
  switch (s) {
    case Source::kCreate:
      return "create";
    case Source::kReuse:
      return "reuse";
    case Source::kRecreate:
      return "recreate";
  }
  return "create";  // unreachable; keeps -Wreturn-type quiet without a default label
}

bool ShouldAttemptCacheRead(Tri exists) {
  return exists != Tri::kNo;
}

Source DecideSource(Tri exists, bool cache_usable) {
  if (cache_usable) {
    return Source::kReuse;
  }
  // Not usable. The only question left is whether there was ever anything to lose.
  switch (exists) {
    case Tri::kNo:
      return Source::kCreate;
    case Tri::kYes:
      // The NameNode holds metadata for a file we cannot read: a replica is gone or the
      // bytes are corrupt. Both are "recreate".
      return Source::kRecreate;
    case Tri::kError:
      // We could not establish that anything was ever written, so claiming we RE-created it
      // would assert a history we have no evidence for. "create" is the weaker, honest claim.
      return Source::kCreate;
  }
  return Source::kCreate;
}

std::string PartitionPath(const std::string& partitions_dir, std::int32_t county_code) {
  std::string dir = partitions_dir;
  while (!dir.empty() && dir.back() == '/') {
    dir.pop_back();
  }
  return dir + "/" + std::to_string(county_code) + ".parquet";
}

}  // namespace lender
