// Parsing for the WebHDFS GETFILEBLOCKLOCATIONS response.
//
// Kept apart from webhdfs.cpp so it can be tested against captured response fixtures with
// no cluster in the loop. The shape being parsed is:
//
//   {"BlockLocations":{"BlockLocation":[
//       {"offset":0,"length":1048576,"corrupt":false,
//        "hosts":["dn-a","dn-b"],
//        "names":["10.0.0.5:9866","10.0.0.6:9866"], ...}, ...]}}
//
// The RPC's contract is "how many blocks are stored by each DataNode", so the answer is a
// histogram over `hosts`, counting each block once per host that holds it. A 2x-replicated
// file therefore contributes 2 to the total across the map, which is the intended reading:
// the map says how much work each node is holding, not how many distinct blocks exist.

#ifndef LENDER_BLOCK_LOCATIONS_HPP
#define LENDER_BLOCK_LOCATIONS_HPP

#include <cstdint>
#include <map>
#include <string>

namespace lender {

// Ordered map: the RPC returns a protobuf map, whose iteration order is unspecified, but a
// deterministic order here makes the tests and the logs readable.
using BlockCounts = std::map<std::string, std::int32_t>;

// Returns false and fills `error` on malformed input. An empty `BlockLocation` array is
// valid and yields an empty map -- an empty file genuinely has no blocks.
bool ParseBlockLocations(const std::string& json, BlockCounts* out, std::string* error);

}  // namespace lender

#endif  // LENDER_BLOCK_LOCATIONS_HPP
