#include <faabric/mpi/mpi.h>
#include <faabric/scheduler/MpiContext.h>
#include <faabric/util/logging.h>

static thread_local faabric::scheduler::MpiContext executingContext;

faabric::scheduler::MpiWorld& getExecutingWorld()
{
    int worldId = executingContext.getWorldId();
    faabric::scheduler::MpiWorldRegistry& reg = faabric::scheduler::getMpiWorldRegistry();
    return reg.getOrInitialiseWorld(*getExecutingCall(), worldId);
}

int MPI_Init(int* argc, char*** argv)
{
    auto logger = faabric::util::getLogger();

    faabric::Message* call = getExecutingCall();

    if (call->mpirank() <= 0) {
        logger->debug("S - MPI_Init (create) {} {}");
        executingContext.createWorld(*call);
    } else {
        logger->debug("S - MPI_Init (join) {} {}");
        executingContext.joinWorld(*call);
    }

    int thisRank = executingContext.getRank();
    faabric::scheduler::MpiWorld& world = getExecutingWorld();
    world.barrier(thisRank);

    return MPI_SUCCESS;
}

int MPI_Comm_rank(MPI_Comm comm, int* rank)
{
    auto logger = faabric::util::getLogger();
    logger->debug("MPI_Comm_rank");

    *rank = 1337;

    return MPI_SUCCESS;
}

int MPI_Comm_size(MPI_Comm comm, int* size)
{
    auto logger = faabric::util::getLogger();
    logger->debug("MPI_Comm_size");

    *size = 9337;

    return MPI_SUCCESS;
}

int MPI_Finalize()
{
    auto logger = faabric::util::getLogger();
    logger->debug("MPI_Finalize");

    return MPI_SUCCESS;
}
