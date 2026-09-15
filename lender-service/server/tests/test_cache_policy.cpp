// Tests for the create / reuse / recreate decision.
//
// This is the table that a live DataNode failure would otherwise be the only way to
// exercise. Every row below corresponds to a real cluster state, named in the comment.

#include "check.hpp"

#include <string>

#include "cache_policy.hpp"
#include "webhdfs.hpp"

using lender::DecideSource;
using lender::ShouldAttemptCacheRead;
using lender::Source;
using lender::Tri;
using lender_test::check;
using lender_test::section;

void test_cache_policy() {
  section("cache policy: which of create/reuse/recreate");

  // Cold cache: nothing has ever asked for this county.
  check(DecideSource(Tri::kNo, false) == Source::kCreate,
        "absent + unread            -> create");

  // Warm cache, healthy cluster: the common case, and the only one that avoids re-reading
  // the whole big file.
  check(DecideSource(Tri::kYes, true) == Source::kReuse,
        "present + readable         -> reuse");

  // THE LOAD-BEARING ROW. The NameNode still holds the metadata, so the file "exists", but
  // its single replica lived on a DataNode that is now down, so nothing can serve the bytes.
  // Reporting 'create' here would claim the cache was never built; 'recreate' is the truth.
  check(DecideSource(Tri::kYes, false) == Source::kRecreate,
        "present + UNREADABLE       -> recreate  (the DataNode-failure case)");

  // Bytes arrived but did not decode. The specification treats corrupt exactly like lost,
  // and the caller folds the decode failure into cache_usable for precisely this reason.
  check(DecideSource(Tri::kYes, false) == Source::kRecreate,
        "present + corrupt bytes    -> recreate  (same branch as lost)");

  // Metadata probe itself failed. We cannot show anything was ever written, so the weaker
  // claim is the honest one.
  check(DecideSource(Tri::kError, false) == Source::kCreate,
        "unknown + unread           -> create    (never assert a history we cannot show)");

  // A successful read settles it regardless of what the metadata probe did.
  check(DecideSource(Tri::kError, true) == Source::kReuse,
        "unknown + readable         -> reuse     (the bytes are proof enough)");

  section("cache policy: whether to attempt the read at all");

  check(!ShouldAttemptCacheRead(Tri::kNo),
        "a confirmed-absent file is not opened (saves a round trip, keeps logs honest)");
  check(ShouldAttemptCacheRead(Tri::kYes), "a present file is opened");
  check(ShouldAttemptCacheRead(Tri::kError),
        "an unknown file IS opened -- the read is the only way left to find out");

  section("cache policy: names on the wire");

  // These three strings are the RPC contract. A typo here is invisible in C++ and breaks
  // every client that branches on the value.
  check(std::string(lender::SourceName(Source::kCreate)) == "create", "kCreate -> 'create'");
  check(std::string(lender::SourceName(Source::kReuse)) == "reuse", "kReuse -> 'reuse'");
  check(std::string(lender::SourceName(Source::kRecreate)) == "recreate",
        "kRecreate -> 'recreate'");

  section("partition paths");

  check(lender::PartitionPath("/partitions", 55025) == "/partitions/55025.parquet",
        "builds /partitions/<code>.parquet");
  check(lender::PartitionPath("/partitions/", 55025) == "/partitions/55025.parquet",
        "a trailing slash does not produce a doubled separator");
  check(lender::PartitionPath("/partitions", 0) == "/partitions/0.parquet",
        "county 0 is a path like any other");
}
