cmake_minimum_required(VERSION 3.13)

#add_library(${PROJECT_NAME}-grpc)

add_library(${PROJECT_NAME}_grpc
   client_grpc.cpp
   service_grpc.cpp
   factory_grpc.cpp
   data_channel_grpc.cpp
)
add_subdirectory (proto)

#target_link_libraries(${PROJECT_NAME}-grpc
#   sisl::sisl
#   nuraft::nuraft
#)
