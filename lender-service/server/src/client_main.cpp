// A native client for the Lender service.
//
// p4/client.py already speaks this protocol and remains the reference client. This one
// exists so the end-to-end check needs nothing but the images this project already builds:
// driving the server from Python otherwise means a Python image plus grpcio plus a codegen
// step, to exercise a service whose stubs are already compiled here.
//
//   lender_client <host:port> DbToHdfs
//   lender_client <host:port> BlockLocations [path]
//   lender_client <host:port> CalcAvgLoan <county_code>
//
// Exit codes are meaningful so a shell script can gate on them: 0 success, 1 usage,
// 2 transport failure, 3 the RPC returned an application-level error.

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "lender.grpc.pb.h"

namespace {

int Usage() {
  std::cerr << "usage: lender_client <host:port> DbToHdfs\n"
            << "       lender_client <host:port> BlockLocations [path]\n"
            << "       lender_client <host:port> CalcAvgLoan <county_code>\n";
  return 1;
}

int Transport(const grpc::Status& s, const char* what) {
  std::cerr << "transport error calling " << what << ": (" << s.error_code() << ") "
            << s.error_message() << "\n";
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return Usage();
  }
  const std::string target = argv[1];
  const std::string mode = argv[2];

  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
  std::unique_ptr<Lender::Stub> stub = Lender::NewStub(channel);

  if (mode == "DbToHdfs") {
    grpc::ClientContext ctx;
    Empty req;
    StatusString resp;
    const grpc::Status s = stub->DbToHdfs(&ctx, req, &resp);
    if (!s.ok()) {
      return Transport(s, "DbToHdfs");
    }
    std::cout << resp.status() << "\n";
    // DbToHdfs has no dedicated error field, so the convention is an "error: " prefix.
    return resp.status().rfind("error:", 0) == 0 ? 3 : 0;
  }

  if (mode == "BlockLocations") {
    grpc::ClientContext ctx;
    BlockLocationsReq req;
    if (argc >= 4) {
      req.set_path(argv[3]);
    }
    BlockLocationsResp resp;
    const grpc::Status s = stub->BlockLocations(&ctx, req, &resp);
    if (!s.ok()) {
      return Transport(s, "BlockLocations");
    }
    if (!resp.error().empty()) {
      std::cout << "error: " << resp.error() << "\n";
      return 3;
    }
    std::int64_t total = 0;
    for (const auto& [host, n] : resp.block_entries()) {
      std::cout << host << " " << n << "\n";
      total += n;
    }
    std::cout << "# " << resp.block_entries().size() << " datanode(s), " << total
              << " block replica(s)\n";
    return 0;
  }

  if (mode == "CalcAvgLoan") {
    if (argc < 4) {
      return Usage();
    }
    grpc::ClientContext ctx;
    CalcAvgLoanReq req;
    req.set_county_code(static_cast<std::int32_t>(std::atoi(argv[3])));
    CalcAvgLoanResp resp;
    const grpc::Status s = stub->CalcAvgLoan(&ctx, req, &resp);
    if (!s.ok()) {
      return Transport(s, "CalcAvgLoan");
    }
    // Matches ../../client.py: a non-empty error means the RPC did not answer, and the
    // answer is not printed. The server keeps non-fatal cache warnings out of this field
    // precisely so the two clients agree.
    if (!resp.error().empty()) {
      std::cout << "error: " << resp.error() << "\n";
      return 3;
    }
    std::cout << resp.avg_loan() << "\n";
    std::cout << resp.source() << "\n";
    return 0;
  }

  return Usage();
}
