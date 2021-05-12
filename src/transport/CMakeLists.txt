# ----------------------------------------------
# Faabric transport library: ZeroMQ + Protobuf
# ----------------------------------------------

set(HEADERS
    "${FAABRIC_INCLUDE_DIR}/faabric/transport/common.h"
    "${FAABRIC_INCLUDE_DIR}/faabric/transport/MessageContext.h"
    "${FAABRIC_INCLUDE_DIR}/faabric/transport/MessageEndpoint.h"
    "${FAABRIC_INCLUDE_DIR}/faabric/transport/MessageEndpointClient.h"
    "${FAABRIC_INCLUDE_DIR}/faabric/transport/MessageEndpointServer.h"
)

set(LIB_FILES 
    MessageContext.cpp
    MessageEndpoint.cpp
    MessageEndpointClient.cpp
    MessageEndpointServer.cpp
    ${HEADERS}
)

faabric_lib(transport "${LIB_FILES}")

# ----------------------------------------------
# Add dependency with ZeroMQ
# ----------------------------------------------

add_dependencies(transport libzeromq_ext)
add_dependencies(transport cppzeromq_ext)

target_link_libraries(transport proto zmq)
