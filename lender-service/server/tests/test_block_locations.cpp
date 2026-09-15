// Tests for the GETFILEBLOCKLOCATIONS parser.
//
// The fixture below is the real response shape from Hadoop 3.3.6's WebHDFS, trimmed to the
// fields the parser reads. Testing against a captured shape rather than one invented to
// match the parser is the point: a parser tested only against its author's idea of the
// format passes for exactly as long as that idea is right.

#include "check.hpp"

#include <string>

#include "block_locations.hpp"

using lender_test::check;
using lender_test::section;

namespace {

// Two blocks, each on two DataNodes -- a 2x-replicated file, which is what DbToHdfs writes.
const char* kTwoBlocksReplicated = R"JSON(
{"BlockLocations":{"BlockLocation":[
  {"cachedHosts":[],"corrupt":false,"hosts":["dn-a","dn-b"],"length":1048576,
   "names":["10.0.0.5:9866","10.0.0.6:9866"],"offset":0,
   "storageIds":["DS-1","DS-2"],"storageTypes":["DISK","DISK"],
   "topologyPaths":["/default-rack/10.0.0.5:9866","/default-rack/10.0.0.6:9866"]},
  {"cachedHosts":[],"corrupt":false,"hosts":["dn-b","dn-c"],"length":524288,
   "names":["10.0.0.6:9866","10.0.0.7:9866"],"offset":1048576,
   "storageIds":["DS-2","DS-3"],"storageTypes":["DISK","DISK"],
   "topologyPaths":["/default-rack/10.0.0.6:9866","/default-rack/10.0.0.7:9866"]}
]}}
)JSON";

const char* kEmpty = R"JSON({"BlockLocations":{"BlockLocation":[]}})JSON";

const char* kRemoteException = R"JSON(
{"RemoteException":{"exception":"FileNotFoundException",
 "javaClassName":"java.io.FileNotFoundException",
 "message":"File does not exist: /nope.parquet"}}
)JSON";

}  // namespace

void test_block_locations() {
  section("block locations parser");

  {
    lender::BlockCounts counts;
    std::string error;
    const bool ok = lender::ParseBlockLocations(kTwoBlocksReplicated, &counts, &error);
    check(ok, "parses a real two-block replicated response");
    check(counts.size() == 3, "three distinct DataNodes appear");
    check(counts["dn-a"] == 1, "dn-a holds 1 block");
    // dn-b holds a replica of BOTH blocks. This is the assertion that would fail if the
    // parser counted distinct blocks instead of per-host holdings.
    check(counts["dn-b"] == 2, "dn-b holds 2 blocks (it replicates both)");
    check(counts["dn-c"] == 1, "dn-c holds 1 block");

    std::int32_t total = 0;
    for (const auto& [host, n] : counts) {
      (void)host;
      total += n;
    }
    check(total == 4, "counts sum to blocks x replicas (2 x 2), not to block count");
  }

  {
    lender::BlockCounts counts;
    std::string error;
    const bool ok = lender::ParseBlockLocations(kEmpty, &counts, &error);
    check(ok, "an empty block list parses successfully");
    check(counts.empty(), "an empty block list yields an empty map, not an error");
  }

  {
    lender::BlockCounts counts;
    std::string error;
    const bool ok = lender::ParseBlockLocations(kRemoteException, &counts, &error);
    check(!ok, "a RemoteException body is reported as an error");
    check(error.find("File does not exist") != std::string::npos,
          "the HDFS message is surfaced, not swallowed into an empty map");
  }

  {
    lender::BlockCounts counts;
    std::string error;
    check(!lender::ParseBlockLocations("this is not json", &counts, &error),
          "malformed JSON is rejected");
    check(!lender::ParseBlockLocations("{}", &counts, &error),
          "JSON without BlockLocations is rejected");
    check(!lender::ParseBlockLocations(R"({"BlockLocations":{"BlockLocation":"x"}})", &counts,
                                       &error),
          "a non-array BlockLocation is rejected");
    check(!lender::ParseBlockLocations(
              R"({"BlockLocations":{"BlockLocation":[{"length":1}]}})", &counts, &error),
          "a block with no 'hosts' is rejected rather than counted as zero");
  }
}
