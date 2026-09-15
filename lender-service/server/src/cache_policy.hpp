// The create / reuse / recreate decision for CalcAvgLoan.
//
// WHY THIS IS ITS OWN FILE, AND PURE. The project stores per-county results at replication
// 1 while the source file sits at replication 2. That asymmetry is deliberate: the cache is
// *meant* to be losable. When a DataNode goes down, some county files become unreadable
// while the big file survives on its second replica, and the server is required to notice,
// rebuild the lost partition, and say which of the three things it did.
//
// Expressed inline in the RPC handler, that logic would only ever be exercisable by killing
// a container. Expressed as a pure function of two observations, every branch -- including
// the ones that only occur during a node failure -- is a unit test that runs on a laptop.
//
// The two observations, and why they are separate:
//
//   exists       what the NameNode says about the file's METADATA (GETFILESTATUS).
//   cache_usable whether the bytes actually came back AND decoded as Parquet.
//
// A downed DataNode makes these disagree: the NameNode still has the metadata, so the file
// "exists", but no replica can serve it, so it is not usable. That disagreement is the
// entire signal for "recreate", which is why collapsing the two into one boolean would
// destroy the distinction the RPC is required to report.

#ifndef LENDER_CACHE_POLICY_HPP
#define LENDER_CACHE_POLICY_HPP

#include <cstdint>
#include <string>

#include "webhdfs.hpp"

namespace lender {

enum class Source { kCreate, kReuse, kRecreate };

// The exact strings the RPC contract specifies.
const char* SourceName(Source s);

// Skip the read entirely when the NameNode says the file is not there -- it saves a round
// trip and, more importantly, keeps a confusing "OPEN failed" out of the logs for what is
// simply a cold cache.
//
// A metadata probe that ERRORED is treated as "might exist": attempting the read is the
// only way left to find out, and a successful read is proof enough.
bool ShouldAttemptCacheRead(Tri exists);

// `cache_usable` must be false unless the bytes were fetched AND decoded. A file that reads
// back as garbage is corrupt, and the specification treats corrupt exactly like lost.
Source DecideSource(Tri exists, bool cache_usable);

// "/partitions" + 55025 -> "/partitions/55025.parquet". Centralised so the writer and the
// reader cannot disagree about it.
std::string PartitionPath(const std::string& partitions_dir, std::int32_t county_code);

}  // namespace lender

#endif  // LENDER_CACHE_POLICY_HPP
