#pragma once

#include <unordered_map>
#include <vector>

#include <sisl/grpc/rpc_server.hpp>
#include <folly/SharedMutex.h>

#include <nuraft_mesg/nuraft_dcs.hpp>
#include <nuraft_mesg/dcs_state_mgr.hpp>
#include <nuraft_mesg/client_factory.hpp>

namespace sisl {
struct io_blob;
}
namespace nuraft_mesg {

using data_lock_type = folly::SharedMutex;

class DataChannelGrpc : public DataChannel {
    // Registered RPCs
    std::vector< std::pair< std::string, sisl::generic_rpc_handler_cb_t > > rpcs_;
    shared< sisl::GrpcServer > grpc_server_;

    std::mutex reg_mtx_;
    std::unordered_map< std::string, rpc_id_t > rpc_names_; // RPC names to its id mapping
    bool rpc_register_done_;

public:
    DataChannelGrpc(shared< ClientFactory > factory, shared< sisl::GrpcServer > grpc_server);
    virtual ~DataChannelGrpc() = default;
    DataChannelGrpc(DataChannelGrpc const&) = delete;
    DataChannelGrpc& operator=(DataChannelGrpc const&) = delete;

    std::pair< rpc_id_t, bool > add_rpc(std::string const& rpc_name, sisl::generic_rpc_handler_cb_t handler) override;
    void close_rpc_registration() override;
    std::vector< std::pair< std::string, sisl::generic_rpc_handler_cb_t > > const& rpcs() const;

    NullAsyncResult unidirectional_request(destination_t const& dest, rpc_id_t rpc_id,
                                           io_blob_list_t const& cli_buf) override;
    AsyncResult< sisl::io_blob > bidirectional_request(destination_t const& dest, rpc_id_t rpc_id,
                                                       io_blob_list_t const& cli_buf) override;

    // Send response to a data channel request and finish the async call.
    void send_response(io_blob_list_t const& outgoing_buf, intrusive< sisl::GenericRpcData >& rpc_data) override;
};

} // namespace nuraft_mesg
