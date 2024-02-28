cmake_minimum_required(VERSION 3.13)

add_subdirectory (grpc)

add_library(${PROJECT_NAME} OBJECT)
settings_gen_cpp($<TARGET_FILE:flatbuffers::flatc> ${CMAKE_CURRENT_BINARY_DIR}/generated/ ${PROJECT_NAME} nuraft_mesg_config.fbs)

target_sources(${PROJECT_NAME} PRIVATE
   manager_impl.cpp
   service.cpp
   factory.cpp
   group_server.cpp
   data_channel.cpp
)
target_link_libraries(${PROJECT_NAME}
   sisl::sisl
   nuraft::nuraft
)