#include <faabric/util/logging.h>

#include <faabric/mpi-native/MpiExecutor.h>
#include <faabric/mpi/mpi.h>

#include <unistd.h>

int main(int argc, char** argv)
{
    auto logger = faabric::util::getLogger();
    auto& scheduler = faabric::scheduler::getScheduler();
    auto& conf = faabric::util::getSystemConfig();

    // Global configuration
    conf.maxNodes = 1;
    conf.maxNodesPerFunction = 1;

    bool __isRoot;
    if (argc < 2) {
        logger->debug("Non-root process started");
        __isRoot = false;
    } else {
        logger->debug("Root process started");
        __isRoot = true;
    }

    // Pre-load message to bootstrap execution
    if (__isRoot) {
        faabric::Message msg = faabric::util::messageFactory("mpi", "exec");
        msg.set_mpiworldsize(2);
        scheduler.callFunction(msg);
    }

    faabric::executor::SingletonPool p;
    p.startPool(false);
}

bool faabric::executor::mpiFunc()
{
    auto logger = faabric::util::getLogger();
    logger->info("Hello world from Faabric MPI Main!");

    MPI_Init(NULL, NULL);

    int rank, worldSize;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &worldSize);

    logger->info("Hello faabric from process {} of {}", rank + 1, worldSize);

    MPI_Finalize();

    return 0;
}
