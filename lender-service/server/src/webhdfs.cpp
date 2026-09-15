#include "webhdfs.hpp"

#include <curl/curl.h>

#include <mutex>
#include <string>

#include "config.hpp"

namespace lender {
namespace {

std::once_flag g_curl_once;

std::size_t AppendToString(const char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  const std::size_t total = size * nmemb;
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, total);
  return total;
}

struct ReadCursor {
  const std::string* data;
  std::size_t offset;
};

std::size_t ReadFromString(char* buffer, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* cursor = static_cast<ReadCursor*>(userdata);
  const std::size_t want = size * nmemb;
  const std::size_t left = cursor->data->size() - cursor->offset;
  const std::size_t n = (want < left) ? want : left;
  if (n > 0) {
    cursor->data->copy(buffer, n, cursor->offset);
    cursor->offset += n;
  }
  return n;
}

// RAII so an early return cannot leak the handle.
class Easy {
 public:
  Easy() : h_(curl_easy_init()) {}
  ~Easy() {
    if (h_ != nullptr) {
      curl_easy_cleanup(h_);
    }
  }
  Easy(const Easy&) = delete;
  Easy& operator=(const Easy&) = delete;

  CURL* get() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

 private:
  CURL* h_;
};

std::string Describe(CURLcode rc, long status) {
  std::string msg = curl_easy_strerror(rc);
  if (rc == CURLE_OK) {
    msg = "HTTP " + std::to_string(status);
  }
  return msg;
}

}  // namespace

void CurlGlobalInitOnce() {
  std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool ValidateHdfsPath(const std::string& path, std::string* error) {
  if (path.empty() || path.front() != '/') {
    *error = "path must be absolute (start with '/'): '" + path + "'";
    return false;
  }
  if (path.size() > 4096) {
    *error = "path too long";
    return false;
  }
  for (const char c : path) {
    const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    const bool punct = (c == '/' || c == '.' || c == '-' || c == '_');
    if (!alnum && !punct) {
      *error = "path contains an unsupported character; allowed: [A-Za-z0-9/._-]";
      return false;
    }
  }
  // Reject traversal outright rather than normalising it. Normalising invites the question
  // of whether our normaliser agrees with the NameNode's.
  if (path.find("..") != std::string::npos) {
    *error = "path must not contain '..'";
    return false;
  }
  return true;
}

WebHdfsClient::WebHdfsClient(const Config& cfg)
    : base_("http://" + cfg.hdfs_host + ":" + std::to_string(cfg.webhdfs_port) + "/webhdfs/v1"),
      user_param_("user.name=" + cfg.hdfs_user),
      timeout_s_(cfg.http_timeout_s) {
  CurlGlobalInitOnce();
}

WebHdfsClient::~WebHdfsClient() = default;

std::string WebHdfsClient::UrlFor(const std::string& path, const std::string& query) const {
  return base_ + path + "?" + user_param_ + "&" + query;
}

Tri WebHdfsClient::Exists(const std::string& path, std::string* error) {
  if (!ValidateHdfsPath(path, error)) {
    return Tri::kError;
  }
  Easy easy;
  if (!easy) {
    *error = "curl_easy_init failed";
    return Tri::kError;
  }
  std::string body;
  const std::string url = UrlFor(path, "op=GETFILESTATUS");
  curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendToString);
  curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, timeout_s_);

  const CURLcode rc = curl_easy_perform(easy.get());
  long status = 0;
  curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK) {
    *error = "GETFILESTATUS " + path + ": " + Describe(rc, status);
    return Tri::kError;
  }
  if (status == 200) {
    return Tri::kYes;
  }
  if (status == 404) {
    return Tri::kNo;
  }
  *error = "GETFILESTATUS " + path + ": HTTP " + std::to_string(status);
  return Tri::kError;
}

bool WebHdfsClient::Read(const std::string& path, std::string* out, std::string* error) {
  if (!ValidateHdfsPath(path, error)) {
    return false;
  }
  Easy easy;
  if (!easy) {
    *error = "curl_easy_init failed";
    return false;
  }
  out->clear();
  const std::string url = UrlFor(path, "op=OPEN");
  curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendToString);
  curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, out);
  // OPEN answers 307 to a DataNode. GET survives a redirect unchanged, so following is safe
  // here in a way it is not for the two-step PUT below.
  curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, timeout_s_);

  const CURLcode rc = curl_easy_perform(easy.get());
  long status = 0;
  curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK || status != 200) {
    // This is the branch a downed DataNode lands in, and CalcAvgLoan depends on being able
    // to tell it apart from a missing file -- so the message names the path and the cause.
    *error = "OPEN " + path + ": " + Describe(rc, status);
    out->clear();
    return false;
  }
  return true;
}

bool WebHdfsClient::Write(const std::string& path,
                          const std::string& data,
                          std::int16_t replication,
                          std::int64_t block_size,
                          std::string* error) {
  if (!ValidateHdfsPath(path, error)) {
    return false;
  }
  // Step 1: ask the NameNode where to put it. WebHDFS answers 307 with a DataNode URL and
  // an empty body. We deliberately do NOT let libcurl follow this automatically: a followed
  // redirect would have to re-send the upload body, and the two-step form is both what the
  // WebHDFS protocol documents and far easier to diagnose when it fails.
  std::string redirect;
  {
    Easy easy;
    if (!easy) {
      *error = "curl_easy_init failed";
      return false;
    }
    std::string discard;
    const std::string url =
        UrlFor(path, "op=CREATE&overwrite=true&replication=" + std::to_string(replication) +
                         "&blocksize=" + std::to_string(block_size));
    curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy.get(), CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendToString);
    curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &discard);
    curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, timeout_s_);

    const CURLcode rc = curl_easy_perform(easy.get());
    long status = 0;
    curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
    if (rc != CURLE_OK) {
      *error = "CREATE(redirect) " + path + ": " + Describe(rc, status);
      return false;
    }
    char* loc = nullptr;
    curl_easy_getinfo(easy.get(), CURLINFO_REDIRECT_URL, &loc);
    if (loc == nullptr) {
      *error = "CREATE(redirect) " + path + ": expected a 307 with a Location, got HTTP " +
               std::to_string(status);
      return false;
    }
    redirect = loc;
  }

  // Step 2: PUT the bytes at the DataNode.
  Easy easy;
  if (!easy) {
    *error = "curl_easy_init failed";
    return false;
  }
  ReadCursor cursor{&data, 0};
  std::string discard;
  curl_easy_setopt(easy.get(), CURLOPT_URL, redirect.c_str());
  curl_easy_setopt(easy.get(), CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(easy.get(), CURLOPT_READFUNCTION, ReadFromString);
  curl_easy_setopt(easy.get(), CURLOPT_READDATA, &cursor);
  curl_easy_setopt(easy.get(), CURLOPT_INFILESIZE_LARGE,
                   static_cast<curl_off_t>(data.size()));
  curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendToString);
  curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &discard);
  curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, timeout_s_);

  const CURLcode rc = curl_easy_perform(easy.get());
  long status = 0;
  curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK || (status != 201 && status != 200)) {
    *error = "CREATE(upload) " + path + ": " + Describe(rc, status);
    return false;
  }
  return true;
}

bool WebHdfsClient::Remove(const std::string& path, std::string* error) {
  if (!ValidateHdfsPath(path, error)) {
    return false;
  }
  Easy easy;
  if (!easy) {
    *error = "curl_easy_init failed";
    return false;
  }
  std::string discard;
  const std::string url = UrlFor(path, "op=DELETE&recursive=false");
  curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy.get(), CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendToString);
  curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, &discard);
  curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, timeout_s_);

  const CURLcode rc = curl_easy_perform(easy.get());
  long status = 0;
  curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK || status != 200) {
    *error = "DELETE " + path + ": " + Describe(rc, status);
    return false;
  }
  return true;
}

bool WebHdfsClient::BlockLocationsJson(const std::string& path, std::string* out,
                                       std::string* error) {
  if (!ValidateHdfsPath(path, error)) {
    return false;
  }
  Easy easy;
  if (!easy) {
    *error = "curl_easy_init failed";
    return false;
  }
  out->clear();
  const std::string url = UrlFor(path, "op=GETFILEBLOCKLOCATIONS");
  curl_easy_setopt(easy.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(easy.get(), CURLOPT_WRITEFUNCTION, AppendToString);
  curl_easy_setopt(easy.get(), CURLOPT_WRITEDATA, out);
  curl_easy_setopt(easy.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy.get(), CURLOPT_TIMEOUT, timeout_s_);

  const CURLcode rc = curl_easy_perform(easy.get());
  long status = 0;
  curl_easy_getinfo(easy.get(), CURLINFO_RESPONSE_CODE, &status);
  if (rc != CURLE_OK || status != 200) {
    *error = "GETFILEBLOCKLOCATIONS " + path + ": " + Describe(rc, status);
    out->clear();
    return false;
  }
  return true;
}

}  // namespace lender
