cmake_minimum_required(VERSION 3.13)

add_library(${PROJECT_NAME}_proto_specs OBJECT
    raft_types.proto
    messaging_service.proto
  )
target_link_libraries(${PROJECT_NAME}_proto_specs
      protobuf::libprotobuf
      gRPC::grpc++
  )

protobuf_generate(LANGUAGE cpp TARGET ${PROJECT_NAME}_proto_specs PROTOS raft_types.proto)
protobuf_generate(LANGUAGE cpp TARGET ${PROJECT_NAME}_proto_specs PROTOS messaging_service.proto)

protobuf_generate(
   TARGET ${PROJECT_NAME}_proto_specs
   LANGUAGE grpc
   GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
   PLUGIN protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
)
target_link_libraries(${PROJECT_NAME}_proto_specs
   protobuf::libprotobuf
   gRPC::grpc++
)

#add_library(${PROJECT_NAME}-proto)
target_sources(${PROJECT_NAME}_grpc PRIVATE
   client_proto.cpp
   service_proto.cpp
   $<TARGET_OBJECTS:${PROJECT_NAME}_proto_specs>
)
target_link_libraries(${PROJECT_NAME}_grpc PUBLIC
   sisl::sisl
   nuraft::nuraft
   protobuf::libprotobuf
   gRPC::grpc++
)
target_include_directories(${PROJECT_NAME}_grpc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
