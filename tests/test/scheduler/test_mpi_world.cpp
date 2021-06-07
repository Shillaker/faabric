#include "faabric_utils.h"
#include <catch.hpp>

#include <faabric/mpi/mpi.h>
#include <faabric/scheduler/FunctionCallServer.h>
#include <faabric/scheduler/MpiWorldRegistry.h>
#include <faabric/scheduler/Scheduler.h>
#include <faabric/util/bytes.h>
#include <faabric/util/macros.h>
#include <faabric/util/network.h>
#include <faabric/util/random.h>

using namespace faabric::scheduler;

static void tearDown(std::vector<MpiWorld*> worlds)
{
    for (auto& world : worlds) {
        world->destroy();
    }

    getScheduler().reset();
}

namespace tests {

static int worldId = 123;
static int worldSize = 10;
static const char* user = "mpi";
static const char* func = "hellompi";

TEST_CASE("Test world creation", "[mpi]")
{
    cleanFaabric();

    Scheduler& sch = getScheduler();

    // Create the world
    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    REQUIRE(world.getSize() == worldSize);
    REQUIRE(world.getId() == worldId);
    REQUIRE(world.getUser() == user);
    REQUIRE(world.getFunction() == func);

    // Check that chained function calls are made as expected
    std::vector<faabric::Message> actual = sch.getRecordedMessagesAll();
    REQUIRE(actual.size() == worldSize - 1);

    for (int i = 0; i < worldSize - 1; i++) {
        faabric::Message actualCall = actual.at(i);
        REQUIRE(actualCall.user() == user);
        REQUIRE(actualCall.function() == func);
        REQUIRE(actualCall.ismpi());
        REQUIRE(actualCall.mpiworldid() == worldId);
        REQUIRE(actualCall.mpirank() == i + 1);
        REQUIRE(actualCall.mpiworldsize() == worldSize);
    }

    // Check that this host is registered as the master
    const std::string actualHost = world.getHostForRank(0);
    REQUIRE(actualHost == faabric::util::getSystemConfig().endpointHost);

    tearDown({ &world });
}

TEST_CASE("Test world loading from msg", "[mpi]")
{
    cleanFaabric();

    // Create a world
    faabric::Message msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld worldA;
    worldA.create(msg, worldId, worldSize);

    // Create another copy from state
    scheduler::MpiWorld worldB;
    // These would be set by the master rank, when invoking other ranks
    msg.set_mpiworldsize(worldSize);
    msg.set_mpiworldid(worldId);
    // Force creating the second world in the _same_ host
    bool forceLocal = true;
    worldB.initialiseFromMsg(msg, forceLocal);

    REQUIRE(worldB.getSize() == worldSize);
    REQUIRE(worldB.getId() == worldId);
    REQUIRE(worldB.getUser() == user);
    REQUIRE(worldB.getFunction() == func);

    tearDown({ &worldA, &worldB });
}

TEST_CASE("Test rank allocation", "[mpi]")
{
    cleanFaabric();

    auto& sch = faabric::scheduler::getScheduler();

    // Force the scheduler to initialise a world in the remote host by setting
    // a worldSize bigger than the slots available locally
    int worldSize = 2;
    faabric::HostResources localResources;
    localResources.set_slots(1);
    localResources.set_usedslots(1);
    faabric::HostResources otherResources;
    otherResources.set_slots(1);

    std::string thisHost = faabric::util::getSystemConfig().endpointHost;
    std::string otherHost = LOCALHOST;
    sch.addHostToGlobalSet(otherHost);

    // Mock everything to make sure the other host has resources as well
    faabric::util::setMockMode(true);
    sch.setThisHostResources(localResources);
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    // Create a world
    faabric::Message msg = faabric::util::messageFactory(user, func);
    msg.set_mpiworldid(worldId);
    msg.set_mpiworldsize(worldSize);

    // Create the local world
    scheduler::MpiWorld& localWorld =
      getMpiWorldRegistry().createWorld(msg, worldId);

    scheduler::MpiWorld remoteWorld;
    remoteWorld.overrideHost(otherHost);
    remoteWorld.initialiseFromMsg(msg);

    // Now check both world instances report the same mappings
    REQUIRE(localWorld.getHostForRank(0) == thisHost);
    REQUIRE(localWorld.getHostForRank(1) == otherHost);

    faabric::util::setMockMode(false);
    tearDown({ &localWorld, &remoteWorld });
}

TEST_CASE("Test cartesian communicator", "[mpi]")
{
    cleanFaabric();

    faabric::Message msg = faabric::util::messageFactory(user, func);

    int worldSize;
    int maxDims = 3;
    std::vector<int> dims(maxDims);
    std::vector<int> periods(2, 1);
    std::vector<std::vector<int>> expectedShift;
    std::vector<std::vector<int>> expectedCoords;

    // Different grid sizes
    SECTION("5 x 1 grid")
    {
        // 5 processes create a 5x1 grid
        worldSize = 5;
        msg.set_mpiworldsize(worldSize);
        dims = { 5, 1, 1 };
        expectedCoords = {
            { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 }, { 3, 0, 0 }, { 4, 0, 0 },
        };
        // We only test for the first three dimensions
        expectedShift = {
            { 4, 1, 0, 0, 0, 0 }, { 0, 2, 1, 1, 1, 1 }, { 1, 3, 2, 2, 2, 2 },
            { 2, 4, 3, 3, 3, 3 }, { 3, 0, 4, 4, 4, 4 },
        };
    }
    SECTION("2 x 2 grid")
    {
        // 4 processes create a 2x2 grid
        worldSize = 4;
        msg.set_mpiworldsize(worldSize);
        dims = { 2, 2, 1 };
        expectedCoords = {
            { 0, 0, 0 },
            { 0, 1, 0 },
            { 1, 0, 0 },
            { 1, 1, 0 },
        };
        // We only test for the first three dimensions
        expectedShift = {
            { 2, 2, 1, 1, 0, 0 },
            { 3, 3, 0, 0, 1, 1 },
            { 0, 0, 3, 3, 2, 2 },
            { 1, 1, 2, 2, 3, 3 },
        };
    }

    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Get coordinates from rank
    for (int i = 0; i < worldSize; i++) {
        std::vector<int> coords(3, -1);
        world.getCartesianRank(
          i, maxDims, dims.data(), periods.data(), coords.data());
        REQUIRE(expectedCoords[i] == coords);
    }

    // Get rank from coordinates
    for (int i = 0; i < dims[0]; i++) {
        for (int j = 0; j < dims[1]; j++) {
            int rank;
            std::vector<int> coords = { i, j, 0 };
            int expected =
              std::find(expectedCoords.begin(), expectedCoords.end(), coords) -
              expectedCoords.begin();
            world.getRankFromCoords(&rank, coords.data());
            REQUIRE(rank == expected);
        }
    }

    // Shift coordinates one unit along each axis
    for (int i = 0; i < dims[0]; i++) {
        for (int j = 0; j < dims[1]; j++) {
            std::vector<int> coords = { i, j, 0 };
            int rank, src, dst;
            world.getRankFromCoords(&rank, coords.data());
            // Test first dimension
            world.shiftCartesianCoords(rank, 0, 1, &src, &dst);
            REQUIRE(src == expectedShift[rank][0]);
            REQUIRE(dst == expectedShift[rank][1]);
            // Test second dimension
            world.shiftCartesianCoords(rank, 1, 1, &src, &dst);
            REQUIRE(src == expectedShift[rank][2]);
            REQUIRE(dst == expectedShift[rank][3]);
            // Test third dimension
            world.shiftCartesianCoords(rank, 2, 1, &src, &dst);
            REQUIRE(src == expectedShift[rank][4]);
            REQUIRE(dst == expectedShift[rank][5]);
        }
    }

    tearDown({ &world });
}

void checkMessage(faabric::MPIMessage& actualMessage,
                  int senderRank,
                  int destRank,
                  const std::vector<int>& data)
{
    // Check the message contents
    REQUIRE(actualMessage.worldid() == worldId);
    REQUIRE(actualMessage.count() == data.size());
    REQUIRE(actualMessage.destination() == destRank);
    REQUIRE(actualMessage.sender() == senderRank);
    REQUIRE(actualMessage.type() == FAABRIC_INT);

    // Check data
    auto* rawInts =
      reinterpret_cast<const int*>(actualMessage.buffer().c_str());
    size_t nInts = actualMessage.buffer().size() / sizeof(int);
    std::vector<int> actualData(rawInts, rawInts + nInts);
    REQUIRE(actualData == data);
}

TEST_CASE("Test send and recv on same host", "[mpi]")
{
    cleanFaabric();

    faabric::Message msg = faabric::util::messageFactory(user, func);
    msg.set_mpiworldsize(2);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Send a message between colocated ranks
    int rankA1 = 0;
    int rankA2 = 1;
    std::vector<int> messageData = { 0, 1, 2 };
    world.send(
      rankA1, rankA2, BYTES(messageData.data()), MPI_INT, messageData.size());

    SECTION("Test queueing")
    {
        // Check the message itself is on the right queue
        REQUIRE(world.getLocalQueueSize(rankA1, rankA2) == 1);
        REQUIRE(world.getLocalQueueSize(rankA2, rankA1) == 0);
        REQUIRE(world.getLocalQueueSize(rankA1, 0) == 0);
        REQUIRE(world.getLocalQueueSize(rankA2, 0) == 0);

        // Check message content
        const std::shared_ptr<InMemoryMpiQueue>& queueA2 =
          world.getLocalQueue(rankA1, rankA2);
        faabric::MPIMessage actualMessage = *(queueA2->dequeue());
        checkMessage(actualMessage, rankA1, rankA2, messageData);
    }

    SECTION("Test recv")
    {
        // Receive the message
        MPI_Status status{};
        auto buffer = new int[messageData.size()];
        world.recv(
          rankA1, rankA2, BYTES(buffer), MPI_INT, messageData.size(), &status);

        std::vector<int> actual(buffer, buffer + messageData.size());
        REQUIRE(actual == messageData);

        REQUIRE(status.MPI_ERROR == MPI_SUCCESS);
        REQUIRE(status.MPI_SOURCE == rankA1);
        REQUIRE(status.bytesSize == messageData.size() * sizeof(int));
    }

    SECTION("Test recv with type missmatch")
    {
        // Receive a message from a different type
        auto buffer = new int[messageData.size()];
        REQUIRE_THROWS(world.recv(rankA1,
                                  rankA2,
                                  BYTES(buffer),
                                  MPI_INT,
                                  messageData.size(),
                                  nullptr,
                                  faabric::MPIMessage::SENDRECV));
    }

    tearDown({ &world });
}

TEST_CASE("Test sendrecv", "[mpi]")
{
    cleanFaabric();

    auto msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Prepare data
    int rankA = 1;
    int rankB = 2;
    MPI_Status status{};
    std::vector<int> messageDataAB = { 0, 1, 2 };
    std::vector<int> messageDataBA = { 3, 2, 1, 0 };

    // Results
    std::vector<int> recvBufferA(messageDataBA.size(), 0);
    std::vector<int> recvBufferB(messageDataAB.size(), 0);

    // sendRecv is blocking, so we run two threads.
    // Run sendrecv from A
    std::vector<std::thread> threads;
    threads.emplace_back([&] {
        world.sendRecv(BYTES(messageDataAB.data()),
                       messageDataAB.size(),
                       MPI_INT,
                       rankB,
                       BYTES(recvBufferA.data()),
                       messageDataBA.size(),
                       MPI_INT,
                       rankB,
                       rankA,
                       &status);
    });
    // Run sendrecv from B
    threads.emplace_back([&] {
        world.sendRecv(BYTES(messageDataBA.data()),
                       messageDataBA.size(),
                       MPI_INT,
                       rankA,
                       BYTES(recvBufferB.data()),
                       messageDataAB.size(),
                       MPI_INT,
                       rankA,
                       rankB,
                       &status);
    });

    // Wait for both to finish
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // Test integrity of results
    REQUIRE(recvBufferA == messageDataBA);
    REQUIRE(recvBufferB == messageDataAB);

    tearDown({ &world });
}

TEST_CASE("Test ring sendrecv", "[mpi]")
{
    cleanFaabric();

    auto msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Use five processes
    std::vector<int> ranks = { 0, 1, 2, 3, 4 };

    // Prepare data
    MPI_Status status{};

    // Run shift operator. In a ring, send to right receive from left.
    std::vector<std::thread> threads;
    for (int i = 0; i < ranks.size(); i++) {
        int rank = ranks[i];
        int left = rank > 0 ? rank - 1 : ranks.size() - 1;
        int right = (rank + 1) % ranks.size();
        threads.emplace_back([&, left, right, i] {
            int recvData = -1;
            int rank = ranks[i];
            world.sendRecv(BYTES(&rank),
                           1,
                           MPI_INT,
                           right,
                           BYTES(&recvData),
                           1,
                           MPI_INT,
                           left,
                           ranks[i],
                           &status);
            // Test integrity of results
            // TODO - no REQUIRE in the test case now
            assert(recvData == left);
        });
    }
    // Wait for all threads to finish
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    tearDown({ &world });
}

TEST_CASE("Test async send and recv", "[mpi]")
{
    cleanFaabric();

    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Send a couple of async messages (from both to each other)
    int rankA = 1;
    int rankB = 2;
    std::vector<int> messageDataA = { 0, 1, 2 };
    std::vector<int> messageDataB = { 3, 4, 5, 6 };
    int sendIdA = world.isend(
      rankA, rankB, BYTES(messageDataA.data()), MPI_INT, messageDataA.size());
    int sendIdB = world.isend(
      rankB, rankA, BYTES(messageDataB.data()), MPI_INT, messageDataB.size());

    // Asynchronously do the receives
    std::vector<int> actualA(messageDataA.size(), 0);
    std::vector<int> actualB(messageDataB.size(), 0);
    int recvIdA =
      world.irecv(rankA, rankB, BYTES(actualA.data()), MPI_INT, actualA.size());
    int recvIdB =
      world.irecv(rankB, rankA, BYTES(actualB.data()), MPI_INT, actualB.size());

    // Await the results out of order (they should all complete)
    world.awaitAsyncRequest(recvIdB);
    world.awaitAsyncRequest(sendIdA);
    world.awaitAsyncRequest(recvIdA);
    world.awaitAsyncRequest(sendIdB);

    REQUIRE(actualA == messageDataA);
    REQUIRE(actualB == messageDataB);

    tearDown({ &world });
}

TEST_CASE("Test send across hosts", "[mpi]")
{
    cleanFaabric();

    // Start a server on this host
    FunctionCallServer server;
    server.start();
    usleep(1000 * 100);

    auto& sch = faabric::scheduler::getScheduler();

    // Force the scheduler to initialise a world in the remote host by setting
    // a worldSize bigger than the slots available locally
    int worldSize = 2;
    faabric::HostResources localResources;
    localResources.set_slots(1);
    localResources.set_usedslots(1);
    faabric::HostResources otherResources;
    otherResources.set_slots(1);

    // Set up a remote host
    std::string otherHost = LOCALHOST;
    sch.addHostToGlobalSet(otherHost);

    // Mock everything to make sure the other host has resources as well
    faabric::util::setMockMode(true);
    sch.setThisHostResources(localResources);
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    // Set up the world on this host
    faabric::Message msg = faabric::util::messageFactory(user, func);
    msg.set_mpiworldid(worldId);
    msg.set_mpiworldsize(worldSize);

    // Create the local world
    scheduler::MpiWorld& localWorld =
      getMpiWorldRegistry().createWorld(msg, worldId);

    scheduler::MpiWorld remoteWorld;
    remoteWorld.overrideHost(otherHost);
    remoteWorld.initialiseFromMsg(msg);

    // Register two ranks (one on each host)
    int rankA = 0;
    int rankB = 1;

    std::vector<int> messageData = { 0, 1, 2 };

    // Undo the mocking, so we actually send the MPI message
    faabric::util::setMockMode(false);

    // Send a message that should get sent to this host
    remoteWorld.send(
      rankB, rankA, BYTES(messageData.data()), MPI_INT, messageData.size());
    usleep(1000 * 100);

    SECTION("Check queueing")
    {
        REQUIRE(localWorld.getLocalQueueSize(rankB, rankA) == 1);

        // Check message content
        faabric::MPIMessage actualMessage =
          *(localWorld.getLocalQueue(rankB, rankA)->dequeue());
        checkMessage(actualMessage, rankB, rankA, messageData);
    }

    SECTION("Check recv")
    {
        // Receive the message for the given rank
        MPI_Status status{};
        auto buffer = new int[messageData.size()];
        localWorld.recv(
          rankB, rankA, BYTES(buffer), MPI_INT, messageData.size(), &status);

        std::vector<int> actual(buffer, buffer + messageData.size());
        REQUIRE(actual == messageData);

        REQUIRE(status.MPI_SOURCE == rankB);
        REQUIRE(status.MPI_ERROR == MPI_SUCCESS);
        REQUIRE(status.bytesSize == messageData.size() * sizeof(int));
    }

    tearDown({ &localWorld, &remoteWorld });

    server.stop();
}

TEST_CASE("Test send/recv message with no data", "[mpi]")
{
    cleanFaabric();

    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    int rankA1 = 1;
    int rankA2 = 2;

    // Send a message between colocated ranks
    std::vector<int> messageData = { 0 };
    world.send(rankA1, rankA2, BYTES(messageData.data()), MPI_INT, 0);

    SECTION("Check on queue")
    {
        // Check message content
        faabric::MPIMessage actualMessage =
          *(world.getLocalQueue(rankA1, rankA2)->dequeue());
        REQUIRE(actualMessage.count() == 0);
        REQUIRE(actualMessage.type() == FAABRIC_INT);
    }

    SECTION("Check receiving with null ptr")
    {
        // Receiving with a null pointer shouldn't break
        MPI_Status status{};
        world.recv(rankA1, rankA2, nullptr, MPI_INT, 0, &status);

        REQUIRE(status.MPI_SOURCE == rankA1);
        REQUIRE(status.MPI_ERROR == MPI_SUCCESS);
        REQUIRE(status.bytesSize == 0);
    }

    tearDown({ &world });
}

TEST_CASE("Test recv with partial data", "[mpi]")
{
    cleanFaabric();

    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Send a message with size less than the recipient is expecting
    std::vector<int> messageData = { 0, 1, 2, 3 };
    unsigned long actualSize = messageData.size();
    world.send(1, 2, BYTES(messageData.data()), MPI_INT, actualSize);

    // Request to receive more values than were sent
    MPI_Status status{};
    unsigned long requestedSize = actualSize + 5;
    auto buffer = new int[requestedSize];
    world.recv(1, 2, BYTES(buffer), MPI_INT, requestedSize, &status);

    // Check status reports only the values that were sent
    REQUIRE(status.MPI_SOURCE == 1);
    REQUIRE(status.MPI_ERROR == MPI_SUCCESS);
    REQUIRE(status.bytesSize == actualSize * sizeof(int));

    tearDown({ &world });
}

TEST_CASE("Test probe", "[mpi]")
{
    cleanFaabric();

    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    // Send two messages of different sizes
    std::vector<int> messageData = { 0, 1, 2, 3, 4, 5, 6 };
    unsigned long sizeA = 2;
    unsigned long sizeB = messageData.size();
    world.send(1, 2, BYTES(messageData.data()), MPI_INT, sizeA);
    world.send(1, 2, BYTES(messageData.data()), MPI_INT, sizeB);

    // Probe twice on the same message
    MPI_Status statusA1{};
    MPI_Status statusA2{};
    MPI_Status statusB{};
    world.probe(1, 2, &statusA1);
    world.probe(1, 2, &statusA2);

    // Check status reports only the values that were sent
    REQUIRE(statusA1.MPI_SOURCE == 1);
    REQUIRE(statusA1.MPI_ERROR == MPI_SUCCESS);
    REQUIRE(statusA1.bytesSize == sizeA * sizeof(int));

    REQUIRE(statusA2.MPI_SOURCE == 1);
    REQUIRE(statusA2.MPI_ERROR == MPI_SUCCESS);
    REQUIRE(statusA2.bytesSize == sizeA * sizeof(int));

    // Receive the message
    auto bufferA = new int[sizeA];
    world.recv(1, 2, BYTES(bufferA), MPI_INT, sizeA * sizeof(int), nullptr);

    // Probe the next message
    world.probe(1, 2, &statusB);
    REQUIRE(statusB.MPI_SOURCE == 1);
    REQUIRE(statusB.MPI_ERROR == MPI_SUCCESS);
    REQUIRE(statusB.bytesSize == sizeB * sizeof(int));

    // Receive the next message
    auto bufferB = new int[sizeB];
    world.recv(1, 2, BYTES(bufferB), MPI_INT, sizeB * sizeof(int), nullptr);

    tearDown({ &world });
}

TEST_CASE("Test can't get in-memory queue for non-local ranks", "[mpi]")
{
    cleanFaabric();

    std::string otherHost = LOCALHOST;

    auto& sch = faabric::scheduler::getScheduler();

    // Force the scheduler to initialise a world in the remote host by setting
    // a worldSize bigger than the slots available locally
    int worldSize = 4;
    faabric::HostResources localResources;
    localResources.set_slots(2);
    localResources.set_usedslots(1);
    faabric::HostResources otherResources;
    otherResources.set_slots(2);

    // Set up a remote host
    sch.addHostToGlobalSet(otherHost);

    // Mock everything to make sure the other host has resources as well
    faabric::util::setMockMode(true);
    sch.setThisHostResources(localResources);
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    faabric::Message msg = faabric::util::messageFactory(user, func);
    msg.set_mpiworldsize(worldSize);
    scheduler::MpiWorld worldA;
    worldA.create(msg, worldId, worldSize);

    scheduler::MpiWorld worldB;
    worldB.overrideHost(otherHost);
    worldB.initialiseFromMsg(msg);

    // Check that we can't access rank on another host locally
    REQUIRE_THROWS(worldA.getLocalQueue(0, 2));

    // Double check even when we've retrieved the rank
    REQUIRE(worldA.getHostForRank(2) == otherHost);
    REQUIRE_THROWS(worldA.getLocalQueue(0, 2));

    faabric::util::setMockMode(false);
    tearDown({ &worldA, &worldB });
}

TEST_CASE("Check sending to invalid rank", "[mpi]")
{
    cleanFaabric();

    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    world.create(msg, worldId, worldSize);

    std::vector<int> input = { 0, 1, 2, 3 };
    int invalidRank = worldSize + 2;
    REQUIRE_THROWS(world.send(0, invalidRank, BYTES(input.data()), MPI_INT, 4));

    tearDown({ &world });
}

TEST_CASE("Test collective messaging locally and across hosts", "[mpi]")
{
    cleanFaabric();

    FunctionCallServer server;
    server.start();
    usleep(1000 * 100);

    auto& sch = faabric::scheduler::getScheduler();

    // Here we rely on the scheduler running out of resources, and overloading
    // the localWorld with ranks 4 and 5
    int thisWorldSize = 6;
    faabric::HostResources localResources;
    localResources.set_slots(1);
    localResources.set_usedslots(1);
    faabric::HostResources otherResources;
    otherResources.set_slots(3);

    // Set up a remote host
    std::string otherHost = LOCALHOST;
    sch.addHostToGlobalSet(otherHost);

    // Mock everything to make sure the other host has resources as well
    faabric::util::setMockMode(true);
    sch.setThisHostResources(localResources);
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    faabric::Message msg = faabric::util::messageFactory(user, func);
    msg.set_mpiworldid(worldId);
    msg.set_mpiworldsize(thisWorldSize);

    MpiWorld& localWorld = getMpiWorldRegistry().createWorld(msg, worldId);

    scheduler::MpiWorld remoteWorld;
    remoteWorld.overrideHost(otherHost);
    remoteWorld.initialiseFromMsg(msg);

    // Unset mock mode to actually send remote MPI messages
    faabric::util::setMockMode(false);

    // Register ranks on both hosts
    int remoteRankA = 1;
    int remoteRankB = 2;
    int remoteRankC = 3;

    int localRankA = 4;
    int localRankB = 5;

    // Note that ranks are deliberately out of order
    std::vector<int> remoteWorldRanks = { remoteRankB,
                                          remoteRankC,
                                          remoteRankA };
    std::vector<int> localWorldRanks = { localRankB, localRankA, 0 };

    SECTION("Broadcast")
    {
        // Broadcast a message
        std::vector<int> messageData = { 0, 1, 2 };
        remoteWorld.broadcast(
          remoteRankB, BYTES(messageData.data()), MPI_INT, messageData.size());

        // Check the host that the root is on
        for (int rank : remoteWorldRanks) {
            if (rank == remoteRankB) {
                continue;
            }

            std::vector<int> actual(3, -1);
            remoteWorld.recv(
              remoteRankB, rank, BYTES(actual.data()), MPI_INT, 3, nullptr);
            REQUIRE(actual == messageData);
        }

        // Check the local host
        for (int rank : localWorldRanks) {
            std::vector<int> actual(3, -1);
            localWorld.recv(
              remoteRankB, rank, BYTES(actual.data()), MPI_INT, 3, nullptr);
            REQUIRE(actual == messageData);
        }
    }

    SECTION("Scatter")
    {
        // Build the data
        int nPerRank = 4;
        int dataSize = nPerRank * worldSize;
        std::vector<int> messageData(dataSize, 0);
        for (int i = 0; i < dataSize; i++) {
            messageData[i] = i;
        }

        // Do the scatter
        std::vector<int> actual(nPerRank, -1);
        remoteWorld.scatter(remoteRankB,
                            remoteRankB,
                            BYTES(messageData.data()),
                            MPI_INT,
                            nPerRank,
                            BYTES(actual.data()),
                            MPI_INT,
                            nPerRank);

        // Check for root
        REQUIRE(actual == std::vector<int>({ 8, 9, 10, 11 }));

        // Check for other remote ranks
        remoteWorld.scatter(remoteRankB,
                            remoteRankA,
                            nullptr,
                            MPI_INT,
                            nPerRank,
                            BYTES(actual.data()),
                            MPI_INT,
                            nPerRank);
        REQUIRE(actual == std::vector<int>({ 4, 5, 6, 7 }));

        remoteWorld.scatter(remoteRankB,
                            remoteRankC,
                            nullptr,
                            MPI_INT,
                            nPerRank,
                            BYTES(actual.data()),
                            MPI_INT,
                            nPerRank);
        REQUIRE(actual == std::vector<int>({ 12, 13, 14, 15 }));

        // Check for local ranks
        localWorld.scatter(remoteRankB,
                           0,
                           nullptr,
                           MPI_INT,
                           nPerRank,
                           BYTES(actual.data()),
                           MPI_INT,
                           nPerRank);
        REQUIRE(actual == std::vector<int>({ 0, 1, 2, 3 }));

        localWorld.scatter(remoteRankB,
                           localRankB,
                           nullptr,
                           MPI_INT,
                           nPerRank,
                           BYTES(actual.data()),
                           MPI_INT,
                           nPerRank);
        REQUIRE(actual == std::vector<int>({ 20, 21, 22, 23 }));

        localWorld.scatter(remoteRankB,
                           localRankA,
                           nullptr,
                           MPI_INT,
                           nPerRank,
                           BYTES(actual.data()),
                           MPI_INT,
                           nPerRank);
        REQUIRE(actual == std::vector<int>({ 16, 17, 18, 19 }));
    }

    SECTION("Gather and allgather")
    {
        // Build the data for each rank
        int nPerRank = 4;
        std::vector<std::vector<int>> rankData;
        for (int i = 0; i < thisWorldSize; i++) {
            std::vector<int> thisRankData;
            for (int j = 0; j < nPerRank; j++) {
                thisRankData.push_back((i * nPerRank) + j);
            }

            rankData.push_back(thisRankData);
        }

        // Build the expectation
        std::vector<int> expected;
        for (int i = 0; i < thisWorldSize * nPerRank; i++) {
            expected.push_back(i);
        }

        SECTION("Gather")
        {
            std::vector<int> actual(thisWorldSize * nPerRank, -1);

            // Call gather for each rank other than the root (out of order)
            int root = localRankA;
            for (int rank : remoteWorldRanks) {
                remoteWorld.gather(rank,
                                   root,
                                   BYTES(rankData[rank].data()),
                                   MPI_INT,
                                   nPerRank,
                                   nullptr,
                                   MPI_INT,
                                   nPerRank);
            }

            for (int rank : localWorldRanks) {
                if (rank == root) {
                    continue;
                }
                localWorld.gather(rank,
                                  root,
                                  BYTES(rankData[rank].data()),
                                  MPI_INT,
                                  nPerRank,
                                  nullptr,
                                  MPI_INT,
                                  nPerRank);
            }

            // Call gather for root
            localWorld.gather(root,
                              root,
                              BYTES(rankData[root].data()),
                              MPI_INT,
                              nPerRank,
                              BYTES(actual.data()),
                              MPI_INT,
                              nPerRank);

            // Check data
            REQUIRE(actual == expected);
        }
    }

    tearDown({ &localWorld, &remoteWorld });

    server.stop();
}

template<typename T>
void doReduceTest(scheduler::MpiWorld& world,
                  int root,
                  MPI_Op op,
                  MPI_Datatype datatype,
                  std::vector<std::vector<T>> rankData,
                  std::vector<T>& expected)
{
    int thisWorldSize = world.getSize();

    bool inPlace;
    SECTION("In place") { inPlace = true; }

    SECTION("Not in place") { inPlace = false; }

    // ---- Reduce ----
    // Call on all but the root first
    for (int r = 0; r < thisWorldSize; r++) {
        if (r == root) {
            continue;
        }
        world.reduce(
          r, root, BYTES(rankData[r].data()), nullptr, datatype, 3, op);
    }

    // Call on root to finish off and check
    std::vector<T> rootRankData = rankData[root];
    if (inPlace) {
        // In-place uses the same buffer for send and receive
        world.reduce(root,
                     root,
                     BYTES(rootRankData.data()),
                     BYTES(rootRankData.data()),
                     datatype,
                     3,
                     op);
        REQUIRE(rootRankData == expected);
    } else {
        // Not in-place uses a separate buffer for send and receive
        std::vector<T> actual(3, 0);
        world.reduce(root,
                     root,
                     BYTES(rootRankData.data()),
                     BYTES(actual.data()),
                     datatype,
                     3,
                     op);
        REQUIRE(actual == expected);
    }

    // ---- Allreduce ----
    // Run all as threads
    std::vector<std::thread> threads;
    for (int r = 0; r < thisWorldSize; r++) {
        threads.emplace_back([&, r, inPlace] {
            std::vector<T> thisRankData = rankData[r];
            if (inPlace) {
                // In-place uses the same buffer for send and receive on _all_
                // hosts
                world.allReduce(r,
                                BYTES(thisRankData.data()),
                                BYTES(thisRankData.data()),
                                datatype,
                                3,
                                op);
                assert(thisRankData == expected);
            } else {
                std::vector<T> actual(3, 0);
                world.allReduce(r,
                                BYTES(thisRankData.data()),
                                BYTES(actual.data()),
                                datatype,
                                3,
                                op);
                assert(actual == expected);
            }
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
}

template void doReduceTest<int>(scheduler::MpiWorld& world,
                                int root,
                                MPI_Op op,
                                MPI_Datatype datatype,
                                std::vector<std::vector<int>> rankData,
                                std::vector<int>& expected);

template void doReduceTest<double>(scheduler::MpiWorld& world,
                                   int root,
                                   MPI_Op op,
                                   MPI_Datatype datatype,
                                   std::vector<std::vector<double>> rankData,
                                   std::vector<double>& expected);

TEST_CASE("Test reduce", "[mpi]")
{
    cleanFaabric();

    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    int thisWorldSize = 5;
    world.create(msg, worldId, thisWorldSize);

    // Prepare inputs
    int root = 3;

    SECTION("Integers")
    {
        std::vector<std::vector<int>> rankData(thisWorldSize,
                                               std::vector<int>(3));
        std::vector<int> expected(3, 0);

        // Prepare rank data
        for (int r = 0; r < thisWorldSize; r++) {
            rankData[r][0] = r;
            rankData[r][1] = r * 10;
            rankData[r][2] = r * 100;
        }

        SECTION("Sum operator")
        {
            for (int r = 0; r < thisWorldSize; r++) {
                expected[0] += rankData[r][0];
                expected[1] += rankData[r][1];
                expected[2] += rankData[r][2];
            }

            doReduceTest<int>(
              world, root, MPI_SUM, MPI_INT, rankData, expected);
        }

        SECTION("Max operator")
        {
            expected[0] = (thisWorldSize - 1);
            expected[1] = (thisWorldSize - 1) * 10;
            expected[2] = (thisWorldSize - 1) * 100;

            doReduceTest<int>(
              world, root, MPI_MAX, MPI_INT, rankData, expected);
        }

        SECTION("Min operator")
        {
            // Initialize rankData to non-zero values. This catches faulty
            // reduce implementations that always return zero
            for (int r = 0; r < thisWorldSize; r++) {
                rankData[r][0] = (r + 1);
                rankData[r][1] = (r + 1) * 10;
                rankData[r][2] = (r + 1) * 100;
            }

            expected[0] = 1;
            expected[1] = 10;
            expected[2] = 100;

            doReduceTest<int>(
              world, root, MPI_MIN, MPI_INT, rankData, expected);
        }
    }

    SECTION("Doubles")
    {
        std::vector<std::vector<double>> rankData(thisWorldSize,
                                                  std::vector<double>(3));
        std::vector<double> expected(3, 0);

        // Prepare rank data
        for (int r = 0; r < thisWorldSize; r++) {
            rankData[r][0] = 2.5 + r;
            rankData[r][1] = (2.5 + r) * 10;
            rankData[r][2] = (2.5 + r) * 100;
        }

        SECTION("Sum operator")
        {
            for (int r = 0; r < thisWorldSize; r++) {
                expected[0] += rankData[r][0];
                expected[1] += rankData[r][1];
                expected[2] += rankData[r][2];
            }

            doReduceTest<double>(
              world, root, MPI_SUM, MPI_DOUBLE, rankData, expected);
        }

        SECTION("Max operator")
        {
            expected[0] = (2.5 + thisWorldSize - 1);
            expected[1] = (2.5 + thisWorldSize - 1) * 10;
            expected[2] = (2.5 + thisWorldSize - 1) * 100;

            doReduceTest<double>(
              world, root, MPI_MAX, MPI_DOUBLE, rankData, expected);
        }

        SECTION("Min operator")
        {
            expected[0] = 2.5;
            expected[1] = 25.0;
            expected[2] = 250.0;

            doReduceTest<double>(
              world, root, MPI_MIN, MPI_DOUBLE, rankData, expected);
        }
    }

    SECTION("Long long")
    {
        std::vector<std::vector<long long>> rankData(thisWorldSize,
                                                     std::vector<long long>(3));
        std::vector<long long> expected(3, 0);

        // Prepare rank data
        for (int r = 0; r < thisWorldSize; r++) {
            rankData[r][0] = (r + 1);
            rankData[r][1] = (r + 1) * 10;
            rankData[r][2] = (r + 1) * 100;
        }

        SECTION("Sum operator")
        {
            for (int r = 0; r < thisWorldSize; r++) {
                expected[0] += rankData[r][0];
                expected[1] += rankData[r][1];
                expected[2] += rankData[r][2];
            }

            doReduceTest<long long>(
              world, root, MPI_SUM, MPI_DOUBLE, rankData, expected);
        }

        SECTION("Max operator")
        {
            expected[0] = thisWorldSize;
            expected[1] = thisWorldSize * 10;
            expected[2] = thisWorldSize * 100;

            doReduceTest<long long>(
              world, root, MPI_MAX, MPI_DOUBLE, rankData, expected);
        }

        SECTION("Min operator")
        {
            expected[0] = 1;
            expected[1] = 10;
            expected[2] = 100;

            doReduceTest<long long>(
              world, root, MPI_MIN, MPI_DOUBLE, rankData, expected);
        }
    }

    tearDown({ &world });
}

TEST_CASE("Test operator reduce", "[mpi]")
{
    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    int thisWorldSize = 5;
    world.create(msg, worldId, thisWorldSize);

    SECTION("Max")
    {
        SECTION("Integers")
        {
            std::vector<int> input = { 1, 1, 1 };
            std::vector<int> output = { 2, 2, 2 };
            std::vector<int> expected = { 2, 2, 2 };

            world.op_reduce(MPI_MAX,
                            MPI_INT,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Doubles")
        {
            std::vector<double> input = { 2, 2, 2 };
            std::vector<double> output = { 1, 1, 1 };
            std::vector<double> expected = { 2, 2, 2 };

            world.op_reduce(MPI_MAX,
                            MPI_DOUBLE,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Long long")
        {
            std::vector<long long> input = { 2, 2, 2 };
            std::vector<long long> output = { 1, 1, 1 };
            std::vector<long long> expected = { 2, 2, 2 };

            world.op_reduce(MPI_MAX,
                            MPI_LONG_LONG,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Unsupported type")
        {
            std::vector<int> input = { 1, 1, 1 };
            std::vector<int> output = { 1, 1, 1 };

            REQUIRE_THROWS(world.op_reduce(MPI_MAX,
                                           MPI_DATATYPE_NULL,
                                           3,
                                           (uint8_t*)input.data(),
                                           (uint8_t*)output.data()));
        }
    }

    SECTION("Min")
    {
        SECTION("Integers")
        {
            std::vector<int> input = { 1, 1, 1 };
            std::vector<int> output = { 2, 2, 2 };
            std::vector<int> expected = { 1, 1, 1 };

            world.op_reduce(MPI_MIN,
                            MPI_INT,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Doubles")
        {
            std::vector<double> input = { 2, 2, 2 };
            std::vector<double> output = { 1, 1, 1 };
            std::vector<double> expected = { 1, 1, 1 };

            world.op_reduce(MPI_MIN,
                            MPI_DOUBLE,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Long long")
        {
            std::vector<long long> input = { 2, 2, 2 };
            std::vector<long long> output = { 1, 1, 1 };
            std::vector<long long> expected = { 1, 1, 1 };

            world.op_reduce(MPI_MIN,
                            MPI_LONG_LONG,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Unsupported type")
        {
            std::vector<int> input = { 1, 1, 1 };
            std::vector<int> output = { 1, 1, 1 };

            REQUIRE_THROWS(world.op_reduce(MPI_MIN,
                                           MPI_DATATYPE_NULL,
                                           3,
                                           (uint8_t*)input.data(),
                                           (uint8_t*)output.data()));
        }
    }

    SECTION("Sum")
    {
        SECTION("Integers")
        {
            std::vector<int> input = { 1, 1, 1 };
            std::vector<int> output = { 1, 1, 1 };
            std::vector<int> expected = { 2, 2, 2 };

            world.op_reduce(MPI_SUM,
                            MPI_INT,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Doubles")
        {
            std::vector<double> input = { 1, 1, 1 };
            std::vector<double> output = { 1, 1, 1 };
            std::vector<double> expected = { 2, 2, 2 };

            world.op_reduce(MPI_SUM,
                            MPI_DOUBLE,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Long long")
        {
            std::vector<long long> input = { 1, 1, 1 };
            std::vector<long long> output = { 1, 1, 1 };
            std::vector<long long> expected = { 2, 2, 2 };

            world.op_reduce(MPI_SUM,
                            MPI_LONG_LONG,
                            3,
                            (uint8_t*)input.data(),
                            (uint8_t*)output.data());
            REQUIRE(output == expected);
        }

        SECTION("Unsupported type")
        {
            std::vector<int> input = { 1, 1, 1 };
            std::vector<int> output = { 1, 1, 1 };

            REQUIRE_THROWS(world.op_reduce(MPI_SUM,
                                           MPI_DATATYPE_NULL,
                                           3,
                                           (uint8_t*)input.data(),
                                           (uint8_t*)output.data()));
        }
    }

    tearDown({ &world });
}

TEST_CASE("Test gather and allgather", "[mpi]")
{
    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    int thisWorldSize = 5;
    int root = 3;

    world.create(msg, worldId, thisWorldSize);

    // Build up per-rank data and expectation
    int nPerRank = 3;
    int gatheredSize = nPerRank * thisWorldSize;
    std::vector<std::vector<int>> rankData(thisWorldSize,
                                           std::vector<int>(nPerRank));
    std::vector<int> expected(gatheredSize, 0);
    for (int i = 0; i < gatheredSize; i++) {
        int thisRank = i / nPerRank;
        expected[i] = i;
        rankData[thisRank][i % nPerRank] = i;
    }

    // Prepare result buffer
    std::vector<int> actual(gatheredSize, 0);

    SECTION("Gather")
    {
        // Run gather on all non-root ranks
        for (int r = 0; r < thisWorldSize; r++) {
            if (r == root) {
                continue;
            }
            world.gather(r,
                         root,
                         BYTES(rankData[r].data()),
                         MPI_INT,
                         nPerRank,
                         nullptr,
                         MPI_INT,
                         nPerRank);
        }

        SECTION("In place")
        {
            // With in-place gather we assume that the root's data is in the
            // correct place in the recv buffer already.
            std::copy(rankData[root].begin(),
                      rankData[root].end(),
                      actual.data() + (root * nPerRank));

            world.gather(root,
                         root,
                         BYTES(actual.data()),
                         MPI_INT,
                         nPerRank,
                         BYTES(actual.data()),
                         MPI_INT,
                         nPerRank);

            REQUIRE(actual == expected);
        }

        SECTION("Not in place")
        {
            world.gather(root,
                         root,
                         BYTES(rankData[root].data()),
                         MPI_INT,
                         nPerRank,
                         BYTES(actual.data()),
                         MPI_INT,
                         nPerRank);

            REQUIRE(actual == expected);
        }
    }

    SECTION("Allgather")
    {
        bool isInPlace;

        SECTION("In place") { isInPlace = true; }

        SECTION("Not in place") { isInPlace = false; }

        // Run allgather in threads
        std::vector<std::thread> threads;
        for (int r = 0; r < thisWorldSize; r++) {
            threads.emplace_back([&, r, isInPlace] {
                if (isInPlace) {
                    // Put this rank's data in place in the recv buffer as
                    // expected
                    std::copy(rankData[r].begin(),
                              rankData[r].end(),
                              actual.data() + (r * nPerRank));

                    world.allGather(r,
                                    BYTES(actual.data()),
                                    MPI_INT,
                                    nPerRank,
                                    BYTES(actual.data()),
                                    MPI_INT,
                                    nPerRank);
                } else {
                    world.allGather(r,
                                    BYTES(rankData[r].data()),
                                    MPI_INT,
                                    nPerRank,
                                    BYTES(actual.data()),
                                    MPI_INT,
                                    nPerRank);
                }

                // TODO remove
                assert(actual == expected);
            });
        }

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        REQUIRE(actual == expected);
    }

    tearDown({ &world });
}

TEST_CASE("Test scan", "[mpi]")
{
    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    int thisWorldSize = 5;
    int count = 3;
    world.create(msg, worldId, thisWorldSize);

    // Prepare input data
    std::vector<std::vector<int>> rankData(thisWorldSize,
                                           std::vector<int>(count));
    for (int r = 0; r < thisWorldSize; r++) {
        for (int i = 0; i < count; i++) {
            rankData[r][i] = r * 10 + i;
        }
    }

    // Prepare expected values
    std::vector<std::vector<int>> expected(thisWorldSize,
                                           std::vector<int>(count));
    for (int r = 0; r < thisWorldSize; r++) {
        for (int i = 0; i < count; i++) {
            if (r == 0) {
                expected[r][i] = rankData[r][i];
            } else {
                expected[r][i] = expected[r - 1][i] + rankData[r][i];
            }
        }
    }

    bool inPlace;
    SECTION("In place") { inPlace = true; }
    SECTION("Not in place") { inPlace = false; }

    // Run the scan operation
    std::vector<std::vector<int>> result(thisWorldSize,
                                         std::vector<int>(count));
    for (int r = 0; r < thisWorldSize; r++) {
        if (inPlace) {
            world.scan(r,
                       BYTES(rankData[r].data()),
                       BYTES(rankData[r].data()),
                       MPI_INT,
                       count,
                       MPI_SUM);
            REQUIRE(rankData[r] == expected[r]);
        } else {
            world.scan(r,
                       BYTES(rankData[r].data()),
                       BYTES(result[r].data()),
                       MPI_INT,
                       count,
                       MPI_SUM);
            REQUIRE(result[r] == expected[r]);
        }
    }

    tearDown({ &world });
}

TEST_CASE("Test all-to-all", "[mpi]")
{
    const faabric::Message& msg = faabric::util::messageFactory(user, func);
    scheduler::MpiWorld world;
    int thisWorldSize = 4;
    world.create(msg, worldId, thisWorldSize);

    // Build inputs and expected
    int inputs[4][8] = {
        { 0, 1, 2, 3, 4, 5, 6, 7 },
        { 10, 11, 12, 13, 14, 15, 16, 17 },
        { 20, 21, 22, 23, 24, 25, 26, 27 },
        { 30, 31, 32, 33, 34, 35, 36, 37 },
    };

    int expected[4][8] = {
        { 0, 1, 10, 11, 20, 21, 30, 31 },
        { 2, 3, 12, 13, 22, 23, 32, 33 },
        { 4, 5, 14, 15, 24, 25, 34, 35 },
        { 6, 7, 16, 17, 26, 27, 36, 37 },
    };

    std::vector<std::thread> threads;
    for (int r = 0; r < thisWorldSize; r++) {
        threads.emplace_back([&, r] {
            std::vector<int> actual(8, 0);
            world.allToAll(r,
                           BYTES(inputs[r]),
                           MPI_INT,
                           2,
                           BYTES(actual.data()),
                           MPI_INT,
                           2);

            std::vector<int> thisExpected(expected[r], expected[r] + 8);
            assert(actual == thisExpected);
        });
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    tearDown({ &world });
}

TEST_CASE("Test RMA across hosts", "[mpi]")
{
    cleanFaabric();

    auto& sch = faabric::scheduler::getScheduler();

    // Set up host resources
    int worldSize = 5;
    faabric::HostResources localResources;
    localResources.set_slots(3);
    localResources.set_usedslots(1);
    faabric::HostResources otherResources;
    otherResources.set_slots(2);

    // Set up a remote host
    std::string otherHost = LOCALHOST;
    sch.addHostToGlobalSet(otherHost);

    // Mock everything to make sure the other host has resources as well
    faabric::util::setMockMode(true);
    sch.setThisHostResources(localResources);
    faabric::scheduler::queueResourceResponse(otherHost, otherResources);

    faabric::Message msg = faabric::util::messageFactory(user, func);
    msg.set_mpiworldid(worldId);
    msg.set_mpiworldsize(worldSize);

    scheduler::MpiWorld& localWorld =
      getMpiWorldRegistry().createWorld(msg, worldId);

    scheduler::MpiWorld remoteWorld;
    remoteWorld.overrideHost(otherHost);
    remoteWorld.initialiseFromMsg(msg);

    // Undo the mocking, so we actually send remote MPI messages
    faabric::util::setMockMode(false);

    FunctionCallServer server;
    server.start();
    usleep(1000 * 100);

    // Register four ranks
    int rankA1 = 1;
    int rankB1 = 3;

    std::vector<int> dataA1 = { 0, 1, 2, 3 };
    int dataCount = (int)dataA1.size();
    int bufferSize = dataCount * sizeof(int);

    // Create a window
    faabric_win_t winA1{
        .worldId = localWorld.getId(),
        .rank = rankA1,
        .size = bufferSize,
    };
    localWorld.createWindow(winA1.rank, winA1.size, BYTES(dataA1.data()));

    SECTION("RMA Get from another world")
    {
        // Get the window on another host
        std::vector<int> actual = { 0, 0, 0, 0 };
        remoteWorld.rmaGet(
          rankA1, MPI_INT, dataCount, BYTES(actual.data()), MPI_INT, dataCount);
        REQUIRE(actual == dataA1);
    }

    SECTION("RMA Put to another world")
    {
        // Do the put
        std::vector<int> putData = { 10, 11, 12, 13 };
        remoteWorld.rmaPut(rankB1,
                           BYTES(putData.data()),
                           MPI_INT,
                           dataCount,
                           rankA1,
                           MPI_INT,
                           dataCount);
        usleep(1000 * 100);

        // Make sure it's been copied to the memory location
        REQUIRE(dataA1 == putData);

        // Check that getting still works
        std::vector<int> actual = { 0, 0, 0, 0 };
        localWorld.rmaGet(
          rankA1, MPI_INT, dataCount, BYTES(actual.data()), MPI_INT, dataCount);
        REQUIRE(actual == putData);
    }

    tearDown({ &localWorld, &remoteWorld });

    server.stop();
}
}
