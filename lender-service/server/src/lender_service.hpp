// The three RPCs of the Lender service.

#ifndef LENDER_SERVICE_HPP
#define LENDER_SERVICE_HPP

#include <grpcpp/grpcpp.h>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "config.hpp"
#include "lender.grpc.pb.h"
#include "webhdfs.hpp"

namespace lender {

class LenderServiceImpl final : public Lender::Service {
 public:
  // The HDFS client is injected rather than constructed internally so tests can drive every
  // RPC -- including the DataNode-failure paths -- against a fake. Borrowed, not owned: it
  // outlives the service in main().
  LenderServiceImpl(Config cfg, HdfsClient* hdfs);

  grpc::Status DbToHdfs(grpc::ServerContext* ctx,
                        const Empty* req,
                        StatusString* resp) override;

  grpc::Status BlockLocations(grpc::ServerContext* ctx,
                              const BlockLocationsReq* req,
                              BlockLocationsResp* resp) override;

  grpc::Status CalcAvgLoan(grpc::ServerContext* ctx,
                           const CalcAvgLoanReq* req,
                           CalcAvgLoanResp* resp) override;

 private:
  // gRPC dispatches handlers on a thread pool, so two clients can ask for the same county
  // at the same moment. Both would find no cache, both would rebuild it, and both would PUT
  // to the same HDFS path -- wasted work, and two writers racing on one path.
  //
  // Striping by county rather than one global lock keeps different counties genuinely
  // parallel, which matters because the slow part is reading the whole big file.
  std::mutex& LockForCounty(std::int32_t county_code);

  Config cfg_;
  HdfsClient* hdfs_;

  std::mutex big_file_mu_;  // serialises DbToHdfs against itself

  std::mutex stripe_table_mu_;
  std::map<std::int32_t, std::unique_ptr<std::mutex>> county_mu_;
};

}  // namespace lender

#endif  // LENDER_SERVICE_HPP
