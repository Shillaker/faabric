#include <faabric/transport/Message.h>

namespace faabric::transport {
Message::Message(const zmq::message_t& msgIn)
  : _size(msgIn.size())
  , _more(msgIn.more())
  , _persist(false)
{
    msg = reinterpret_cast<uint8_t*>(malloc(_size * sizeof(uint8_t)));
    memcpy(msg, msgIn.data(), _size);
}

Message::Message(int sizeIn)
  : _size(sizeIn)
  , _more(false)
  , _persist(false)
{
    msg = reinterpret_cast<uint8_t*>(malloc(_size * sizeof(uint8_t)));
}

// Empty message signals shutdown
Message::Message()
  : msg(nullptr)
{}

Message::~Message()
{
    if (!_persist) {
        free(reinterpret_cast<void*>(msg));
    }
}

char* Message::data()
{
    return reinterpret_cast<char*>(msg);
}

uint8_t* Message::udata()
{
    return msg;
}

int Message::size()
{
    return _size;
}

bool Message::more()
{
    return _more;
}

void Message::persist()
{
    _persist = true;
}
}
