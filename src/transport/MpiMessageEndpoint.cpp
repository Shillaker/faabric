#include <faabric/transport/MpiMessageEndpoint.h>

namespace faabric::transport {
faabric::MpiHostsToRanksMessage recvMpiHostRankMsg()
{
    faabric::transport::RecvMessageEndpoint endpoint(MPI_PORT);
    endpoint.open(faabric::transport::getGlobalMessageContext());
    faabric::transport::Message m = endpoint.recv();
    PARSE_MSG(faabric::MpiHostsToRanksMessage, m.data(), m.size());
    endpoint.close();

    return msg;
}

void sendMpiHostRankMsg(const std::string& hostIn,
                        const faabric::MpiHostsToRanksMessage msg)
{
    size_t msgSize = msg.ByteSizeLong();
    {
        uint8_t sMsg[msgSize];
        if (!msg.SerializeToArray(sMsg, msgSize)) {
            throw std::runtime_error("Error serialising message");
        }
        faabric::transport::SendMessageEndpoint endpoint(hostIn, MPI_PORT);
        endpoint.open(faabric::transport::getGlobalMessageContext());
        endpoint.send(sMsg, msgSize, false);
        endpoint.close();
    }
}

MpiMessageEndpoint::MpiMessageEndpoint(const std::string& hostIn, int portIn)
  : sendMessageEndpoint(hostIn, portIn)
  , recvMessageEndpoint(portIn)
{}

void MpiMessageEndpoint::sendMpiMessage(
  const std::shared_ptr<faabric::MPIMessage>& msg)
{
    // TODO - is this lazy init very expensive?
    if (sendMessageEndpoint.socket == nullptr) {
        sendMessageEndpoint.open(faabric::transport::getGlobalMessageContext());
    }

    size_t msgSize = msg->ByteSizeLong();
    {
        uint8_t sMsg[msgSize];
        if (!msg->SerializeToArray(sMsg, msgSize)) {
            throw std::runtime_error("Error serialising message");
        }
        sendMessageEndpoint.send(sMsg, msgSize, false);
    }
}

std::shared_ptr<faabric::MPIMessage> MpiMessageEndpoint::recvMpiMessage()
{
    if (recvMessageEndpoint.socket == nullptr) {
        recvMessageEndpoint.open(faabric::transport::getGlobalMessageContext());
    }

    Message m = recvMessageEndpoint.recv();
    PARSE_MSG(faabric::MPIMessage, m.data(), m.size());

    return std::make_shared<faabric::MPIMessage>(msg);
}

void MpiMessageEndpoint::close()
{
    if (sendMessageEndpoint.socket != nullptr) {
        sendMessageEndpoint.close();
    }
    if (recvMessageEndpoint.socket != nullptr) {
        recvMessageEndpoint.close();
    }
}
}
