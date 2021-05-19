#include <faabric/scheduler/SnapshotClient.h>
#include <faabric/util/logging.h>
#include <faabric/util/queue.h>
#include <faabric/util/testing.h>

namespace faabric::scheduler {

// -----------------------------------
// Mocking
// -----------------------------------

static std::vector<std::pair<std::string, faabric::util::SnapshotData>>
  snapshotPushes;

static std::vector<std::pair<std::string, std::string>> snapshotDeletes;

std::vector<std::pair<std::string, faabric::util::SnapshotData>>
getSnapshotPushes()
{
    return snapshotPushes;
}

std::vector<std::pair<std::string, std::string>> getSnapshotDeletes()
{
    return snapshotDeletes;
}

void clearMockSnapshotRequests()
{
    snapshotPushes.clear();
    snapshotDeletes.clear();
}

// -----------------------------------
// Snapshot client
// -----------------------------------

SnapshotClient::SnapshotClient(const std::string& hostIn)
  : faabric::transport::MessageEndpointClient(hostIn, SNAPSHOT_PORT)
{
    this->open(faabric::transport::getGlobalMessageContext());
}

void SnapshotClient::sendHeader(faabric::scheduler::SnapshotCalls call)
{
    uint8_t header = static_cast<uint8_t>(call);
    send(&header, sizeof(header), true);
}

void SnapshotClient::pushSnapshot(const std::string& key,
                                  const faabric::util::SnapshotData& req)
{
    auto logger = faabric::util::getLogger();
    logger->debug("Pushing snapshot {} to {}", key, host);

    if (faabric::util::isMockMode()) {
        snapshotPushes.emplace_back(host, req);
    } else {
        // Send the header first
        sendHeader(faabric::scheduler::SnapshotCalls::PushSnapshot);

        // TODO - avoid copying data here
        flatbuffers::FlatBufferBuilder mb;
        auto keyOffset = mb.CreateString(key);
        auto dataOffset = mb.CreateVector<uint8_t>(req.data, req.size);
        auto requestOffset =
          CreateSnapshotPushRequest(mb, keyOffset, dataOffset);

        mb.Finish(requestOffset);
        uint8_t* msg = mb.GetBufferPointer();
        int size = mb.GetSize();
        send(msg, size);
    }
}

void SnapshotClient::deleteSnapshot(const std::string& key)
{
    auto logger = faabric::util::getLogger();

    if (faabric::util::isMockMode()) {
        snapshotDeletes.emplace_back(host, key);
    } else {
        logger->debug("Deleting snapshot {} from {}", key, host);

        // Send the header first
        sendHeader(faabric::scheduler::SnapshotCalls::DeleteSnapshot);

        // TODO - avoid copying data here
        flatbuffers::FlatBufferBuilder mb;
        auto keyOffset = mb.CreateString(key);
        auto requestOffset = CreateSnapshotDeleteRequest(mb, keyOffset);

        mb.Finish(requestOffset);
        uint8_t* msg = mb.GetBufferPointer();
        int size = mb.GetSize();
        send(msg, size);
    }
}
}
