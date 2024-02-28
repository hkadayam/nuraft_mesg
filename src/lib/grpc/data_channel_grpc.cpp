#include <boost/uuid/uuid_io.hpp>
#include <sisl/grpc/generic_service.hpp>
#include <sisl/fds/vector_pool.hpp>

#include "data_channel_grpc.hpp"
#include "lib/common_lib.hpp"
#include "lib/grpc/client_grpc.hpp"
#include "lib/generated/nuraft_mesg_config_generated.h"

namespace nuraft_mesg {

DataChannelGrpc::DataChannelGrpc(shared< ClientFactory > factory, shared< sisl::GrpcServer > grpc_server) :
        DataChannel(std::move(factory)), grpc_server_{std::move(grpc_server)}, rpc_register_done_{false} {}

std::pair< rpc_id_t, bool > DataChannelGrpc::add_rpc(std::string const& rpc_name,
                                                     sisl::generic_rpc_handler_cb_t handler) {
    std::unique_lock lg(reg_mtx_);
    if (rpc_register_done_) {
        LOGW("RPC registration after registration is closed, no more register");
        return std::pair(invalid_rpc_id, false);
    }

    rpc_id_t id = static_cast< int64_t >(rpcs_.size());
    auto [it, happened] = rpc_names_.insert(std::pair(rpc_name, id));
    if (!happened) { return std::pair(it->second, true); }

    rpcs_.emplace_back(std::pair(rpc_name, std::move(handler)));
    return std::pair(id, false);
}

void DataChannelGrpc::close_rpc_registration() {
    std::unique_lock lg(reg_mtx_);
    rpc_register_done_ = true;
}

std::vector< std::pair< std::string, sisl::generic_rpc_handler_cb_t > > const& DataChannelGrpc::rpcs() const {
    return rpcs_;
}

NullAsyncResult DataChannelGrpc::unidirectional_request(destination_t const& dest, rpc_id_t rpc_id,
                                                        io_blob_list_t const& cli_buf) {
    grpc::ByteBuffer cli_byte_buf;
    serialize_to_byte_buffer(cli_byte_buf, cli_buf);

    auto send_rpc = [this](shared< DCSClient > client, std::string const& rpc_name,
                           grpc::ByteBuffer const& cli_byte_buf) {
        return std::static_pointer_cast< DCSClientGrpc >(client)
            ->generic_stub()
            .call_unary(cli_byte_buf, rpc_name, NURAFT_MESG_CONFIG(mesg_factory_config->data_request_deadline_secs))
            .deferValue([](auto&& response) -> NullResult {
                if (response.hasError()) {
                    LOGE("Failed to send data_service_request, error: {}", response.error().error_message());
                    return folly::makeUnexpected(nuraft::cmd_result_code::CANCELLED);
                }
                return folly::unit;
            });
    };

    std::shared_lock rl(peer_mtx_);
    std::optional< Result< peer_id_t > > peer = get_peer_id(dest);

    if (peer) {
        if (peer->hasError()) { return folly::makeUnexpected(peer->error()); }

        if (auto it = peer_clients_.find(peer->value()); it != peer_clients_.end()) {
            return send_rpc(it->second, rpcs_[rpc_id].first, cli_byte_buf);
        } else {
            LOGE("Failed to find client for rpc_name [{}]", rpcs_[rpc_id].first);
            return folly::makeUnexpected(nuraft::cmd_result_code::SERVER_NOT_FOUND);
        }
    } else {
        auto calls = sisl::VectorPool< NullAsyncResult >::alloc();
        for (auto& dcs_client : peer_clients_) {
            calls->push_back(send_rpc(dcs_client, rpcs_[rpc_id].first, cli_byte_buf));
        }

        return folly::collectAll(*calls).deferValue([calls](auto&&) -> NullResult {
            sisl::VectorPool< NullAsyncResult >::free(calls);
            return folly::unit;
        });
    }
}

AsyncResult< sisl::io_blob > DataChannelGrpc::bidirectional_request(destination_t const& dest, rpc_id_t rpc_id,
                                                                    io_blob_list_t const& cli_buf) {
    grpc::ByteBuffer cli_byte_buf;
    serialize_to_byte_buffer(cli_byte_buf, cli_buf);

    auto send_rpc = [this](shared< DCSClient > client, std::string const& rpc_name,
                           grpc::ByteBuffer const& cli_byte_buf) {
        return std::static_pointer_cast< DCSClientGrpc >(client)
            ->generic_stub()
            .call_unary(cli_byte_buf, rpc_name, NURAFT_MESG_CONFIG(mesg_factory_config->data_request_deadline_secs))
            .deferValue([](auto&& response) -> Result< sisl::io_blob > {
                if (response.hasError()) {
                    LOGE("Failed to send data service rpc, error: {}", response.error().error_message());
                    return folly::makeUnexpected(nuraft::cmd_result_code::CANCELLED);
                }
                sisl::io_blob svr_buf;
                deserialize_from_byte_buffer(response.value(), svr_buf);
                return svr_buf;
            });
    };

    std::shared_lock rl(peer_mtx_);
    std::optional< Result< peer_id_t > > peer = get_peer_id(dest);

    if (peer) {
        if (peer->hasError()) { return folly::makeUnexpected(peer->error()); }

        if (auto it = peer_clients_.find(peer->value()); it != peer_clients_.end()) {
            return send_rpc(it->second, rpcs_[rpc_id].first, cli_buf);
        } else {
            LOGE("Failed to find client rpc_name [{}]", rpcs_[rpc_id].first);
            return folly::makeUnexpected(nuraft::cmd_result_code::SERVER_NOT_FOUND);
        }
    } else {
        LOGE("Bidirectional requests to multiple nodes not implemented yet!. rpc_name [{}]", rpcs_[rpc_id].first);
        return folly::makeUnexpected(nuraft::cmd_result_code::BAD_REQUEST);
    }
}

void DataChannelGrpc::send_response(io_blob_list_t const& outgoing_buf, intrusive< sisl::GenericRpcData >& rpc_data) {
    serialize_to_byte_buffer(rpc_data->response(), outgoing_buf);
    rpc_data->send_response();
}
} // namespace nuraft_mesg
