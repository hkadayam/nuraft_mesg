#include <boost/uuid/uuid_io.hpp>
#include <sisl/grpc/generic_service.hpp>

#include "DataChannelGrpc.hpp"
#include "common_lib.hpp"

namespace nuraft_mesg {

DataChannelGrpc::DataChannelGrpc(shared< ClientFactory > factory, shared< sisl::GrpcServer > grpc_server) :
        DataChannel(std::move(factory)), grpc_server_{std::move(server)} {}

} // namespace nuraft_mesg
