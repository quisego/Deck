set(PROTO_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../common/proto)

idf_component_register(
  SRCS
    ${PROTO_DIR}/src/protocol.c
    ${PROTO_DIR}/src/msg.c
  INCLUDE_DIRS
    ${PROTO_DIR}/include
)