#include "block_locations.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace lender {

bool ParseBlockLocations(const std::string& json, BlockCounts* out, std::string* error) {
  out->clear();

  nlohmann::json doc = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    *error = "GETFILEBLOCKLOCATIONS response is not valid JSON";
    return false;
  }

  // WebHDFS reports failures as a RemoteException object with a 200-shaped body in some
  // deployments, so surface that message instead of reporting "no blocks".
  if (doc.contains("RemoteException")) {
    const auto& ex = doc["RemoteException"];
    const std::string msg = ex.value("message", std::string("unspecified RemoteException"));
    *error = "HDFS: " + msg;
    return false;
  }

  if (!doc.contains("BlockLocations")) {
    *error = "response has no 'BlockLocations' key";
    return false;
  }
  const auto& wrapper = doc["BlockLocations"];
  if (!wrapper.is_object() || !wrapper.contains("BlockLocation")) {
    *error = "'BlockLocations' has no 'BlockLocation' array";
    return false;
  }
  const auto& blocks = wrapper["BlockLocation"];
  if (!blocks.is_array()) {
    *error = "'BlockLocation' is not an array";
    return false;
  }

  for (const auto& block : blocks) {
    if (!block.is_object()) {
      *error = "a BlockLocation entry is not an object";
      return false;
    }
    if (!block.contains("hosts")) {
      *error = "a BlockLocation entry has no 'hosts'";
      return false;
    }
    const auto& hosts = block["hosts"];
    if (!hosts.is_array()) {
      *error = "'hosts' is not an array";
      return false;
    }
    for (const auto& host : hosts) {
      if (!host.is_string()) {
        *error = "a 'hosts' entry is not a string";
        return false;
      }
      // operator[] value-initialises a missing key to 0, so this counts correctly on the
      // first sighting of a host as well as on later ones.
      (*out)[host.get<std::string>()] += 1;
    }
  }
  return true;
}

}  // namespace lender
