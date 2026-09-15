// HDFS access over the WebHDFS REST API.
//
// WHY REST RATHER THAN libhdfs. libhdfs is a JNI shim: linking it obliges the *runtime*
// image to carry a JVM and a correct CLASSPATH, which roughly triples the server image for
// a program that makes four kinds of call. WebHDFS is plain HTTP on the NameNode's port
// 9870, and libcurl is already in the toolchain. The cost is that every operation is a
// round trip and large reads stream through memory -- acceptable at this dataset's size,
// and called out in the README rather than hidden.
//
// The interface is abstract on purpose. The create/reuse/recreate decision in
// cache_policy.hpp is the only genuinely subtle logic in this server, and it is decided
// entirely by what HDFS answers to three questions. Against an abstract client that logic
// is testable on a laptop with no cluster at all; against a concrete one it would only ever
// be testable by killing DataNodes.

#ifndef LENDER_WEBHDFS_HPP
#define LENDER_WEBHDFS_HPP

#include <cstdint>
#include <string>

namespace lender {

struct Config;

// Three-valued because "the file is not there" and "I could not find out" are different
// answers that lead to different RPC behaviour, and collapsing them into a bool is exactly
// how a failed probe gets read as a clean negative.
enum class Tri { kYes, kNo, kError };

class HdfsClient {
 public:
  virtual ~HdfsClient() = default;

  // Does `path` exist? kError means the question could not be answered.
  virtual Tri Exists(const std::string& path, std::string* error) = 0;

  // Whole-file read into memory.
  virtual bool Read(const std::string& path, std::string* out, std::string* error) = 0;

  // Create or overwrite `path` with the given durability settings.
  virtual bool Write(const std::string& path,
                     const std::string& data,
                     std::int16_t replication,
                     std::int64_t block_size,
                     std::string* error) = 0;

  virtual bool Remove(const std::string& path, std::string* error) = 0;

  // Raw GETFILEBLOCKLOCATIONS response body; parsing lives in block_locations.hpp so it can
  // be tested against captured fixtures.
  virtual bool BlockLocationsJson(const std::string& path, std::string* out,
                                  std::string* error) = 0;
};

class WebHdfsClient : public HdfsClient {
 public:
  explicit WebHdfsClient(const Config& cfg);
  ~WebHdfsClient() override;

  WebHdfsClient(const WebHdfsClient&) = delete;
  WebHdfsClient& operator=(const WebHdfsClient&) = delete;

  Tri Exists(const std::string& path, std::string* error) override;
  bool Read(const std::string& path, std::string* out, std::string* error) override;
  bool Write(const std::string& path,
             const std::string& data,
             std::int16_t replication,
             std::int64_t block_size,
             std::string* error) override;
  bool Remove(const std::string& path, std::string* error) override;
  bool BlockLocationsJson(const std::string& path, std::string* out,
                          std::string* error) override;

 private:
  std::string base_;       // http://host:port/webhdfs/v1
  std::string user_param_; // &user.name=...
  long timeout_s_;

  std::string UrlFor(const std::string& path, const std::string& query) const;
};

// An HDFS path is attacker-controllable: BlockLocations takes one straight off the wire.
// Returns false and fills `error` for anything that is not a plain absolute path, which
// keeps traversal and query-injection out of the URL we are about to build. Exposed for
// testing.
bool ValidateHdfsPath(const std::string& path, std::string* error);

// Initialises libcurl exactly once per process. Calling it more than once is harmless.
void CurlGlobalInitOnce();

}  // namespace lender

#endif  // LENDER_WEBHDFS_HPP
