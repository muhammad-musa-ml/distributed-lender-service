// Entry point for the Lender gRPC server.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <csignal>
#include <iostream>
#include <memory>
#include <string>

#include "config.hpp"
#include "lender_service.hpp"
#include "webhdfs.hpp"

namespace {

std::atomic<bool> g_stop{false};
grpc::Server* g_server = nullptr;

// Compose sends SIGTERM and waits ~10s before SIGKILL. Shutting down deliberately lets
// in-flight RPCs finish instead of having their connections cut mid-response.
extern "C" void HandleSignal(int) {
  g_stop.store(true);
  if (g_server != nullptr) {
    g_server->Shutdown();
  }
}

}  // namespace

int main() {
  const lender::Config cfg = lender::config_from_env();

  lender::CurlGlobalInitOnce();
  lender::WebHdfsClient hdfs(cfg);
  lender::LenderServiceImpl service(cfg, &hdfs);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(cfg.listen_addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (server == nullptr) {
    std::cerr << "fatal: could not bind " << cfg.listen_addr << "\n";
    return 1;
  }
  g_server = server.get();

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  // One startup line naming every resolved endpoint. When this server misbehaves in compose
  // the first question is always "what did it actually connect to", and answering it in the
  // logs costs nothing.
  std::cout << "lender-server listening on " << cfg.listen_addr << "\n"
            << "  mysql   : " << cfg.mysql_user << "@" << cfg.mysql_host << ":"
            << cfg.mysql_port << "/" << cfg.mysql_db << "\n"
            << "  webhdfs : http://" << cfg.hdfs_host << ":" << cfg.webhdfs_port << "\n"
            << "  big file: " << cfg.big_parquet_path << " (replication "
            << cfg.big_replication << ", block " << cfg.big_block_size << ")\n"
            << "  parts   : " << cfg.partitions_dir << " (replication "
            << cfg.partition_replication << ")\n"
            << std::flush;

  server->Wait();
  std::cout << "lender-server stopped\n";
  return 0;
}
