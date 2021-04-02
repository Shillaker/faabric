#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/channel_arguments.h>

#include <faabric/flat/faabric.grpc.fb.h>
#include <faabric/flat/faabric_generated.h>
#include <faabric/util/snapshot.h>

using namespace grpc;

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;

namespace faabric::scheduler {

// -----------------------------------
// Mocking
// -----------------------------------
std::vector<std::pair<std::string, faabric::util::SnapshotData>>
getSnapshotPushes();

void clearMockSnapshotRequests();

// -----------------------------------
// gRPC client
// -----------------------------------
class SnapshotClient
{
  public:
    explicit SnapshotClient(const std::string& hostIn);

    const std::string host;

    std::shared_ptr<Channel> channel;
    std::unique_ptr<SnapshotService::Stub> stub;

    void pushSnapshot(const std::string &key, const faabric::util::SnapshotData& data);
};
}
