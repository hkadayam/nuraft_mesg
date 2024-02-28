#include <sisl/grpc/rpc_server.hpp>
#include <sisl/grpc/generic_service.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

#include "lib/grpc/service_grpc.hpp"
#include "lib/grpc/factory_grpc.hpp"
#include "lib/grpc/data_channel_grpc.hpp"

namespace nuraft_mesg {
DCSRaftServiceGrpc::DCSRaftServiceGrpc(shared< DCSManagerImpl > manager, DCSManager::Params const& params) :
        DCSRaftService(std::move(manager), params) {}

void DCSRaftServiceGrpc::start() {
    auto listen_address = fmt::format(FMT_STRING("0.0.0.0:{}"), params_.mesg_port);
    LOGI("Starting Data Consensus Raft Service with GRPC on http://{}", listen_address);

    grpc_server_.reset();
    grpc_server_ = shared< sisl::GrpcServer >(sisl::GrpcServer::make(
        listen_address, params_.token_verifier, params_.rpc_server_threads, params_.ssl_key, params_.ssl_cert));

    grpc_server_->run();

    factory_ =
        std::make_shared< ClientFactoryGrpc >(params_.rpc_client_threads, boost::uuids::to_string(params_.server_uuid),
                                              manager_.lock()->application(), params_.token_client, params_.ssl_cert);
}

void DCSRaftServiceGrpc::shutdown() {
    grpc_server_->shutdown();
    DCSRaftService::shutdown();
}

/////////////////////////////// DCSDataServiceGrpc Section ///////////////////////////////////////////
DCSDataServiceGrpc::DCSDataServiceGrpc(shared< DCSManagerImpl > manager, DCSManager::Params const& params,
                                       shared< DCSRaftServiceGrpc > raft_service) {
    if (raft_service) { grpc_server_ = raft_service->server(); }

    if (!grpc_server_) {
        auto listen_address = fmt::format(FMT_STRING("0.0.0.0:{}"), params.mesg_port);
        LOGI("Starting Data Consensus Data Service with GRPC on http://{}", listen_address);

        grpc_server_ = shared< sisl::GrpcServer >(sisl::GrpcServer::make(
            listen_address, params.token_verifier, params.rpc_server_threads, params.ssl_key, params.ssl_cert));

        grpc_server_->run();
    } else {
        LOGI("Using Data Consensus Raft Service with GRPC for Data Service as well");
    }

    if (!grpc_server_->register_async_generic_service()) {
        throw std::runtime_error("Could not register generic service with gRPC!");
    }

    if (raft_service) { factory_ = raft_service->client_factory(); }
    if (!factory_) {
        factory_ = std::make_shared< ClientFactoryGrpc >(params.rpc_client_threads,
                                                         boost::uuids::to_string(params.server_uuid),
                                                         manager->application(), params.token_client, params.ssl_cert);
    }

    start();
}

void DCSDataServiceGrpc::start() { reregister_rpcs(); }

void DCSDataServiceGrpc::shutdown() {
    std::unique_lock lg{map_lock_};
    channel_map_.clear();
}

shared< DataChannel > DCSDataServiceGrpc::create_data_channel(channel_id_t const& channel_id) {
    auto channel = std::make_shared< DataChannelGrpc >(factory_, grpc_server_);

    std::unique_lock lg{map_lock_};
    channel_map_.insert(std::pair(channel_id, channel));
    return channel;
}

void DCSDataServiceGrpc::remove_data_channel(channel_id_t const& channel_id) {
    std::unique_lock lg{map_lock_};
    channel_map_.erase(channel_id);
}

void DCSDataServiceGrpc::reregister_rpcs() {
    std::shared_lock lg{map_lock_};
    for (auto const& [_, channel] : channel_map_) {
        for (auto& [name, handler] : std::static_pointer_cast< DataChannelGrpc >(channel)->rpcs()) {
            if (!grpc_server_->register_generic_rpc(name, handler)) {
                throw std::runtime_error(fmt::format("Could not register generic rpc {} with gRPC!", name));
            }
        }
    }
}

rpc_id_t DCSDataServiceGrpc::register_rpc(std::string const& rpc_name, group_id_t const& group_id,
                                          data_channel_rpc_handler_t const& handler) {
    RELEASE_ASSERT(grpc_server_, "NULL grpc_server!");
    if (!handler) {
        LOGW("Null for the rpc {}, cannot register.", rpc_name);
        return invalid_rpc_id;
    }

    // This is an async call, hence the "return false". The user should invoke rpc_data->send_response to finish the
    // call
    auto generic_handler_cb = [handler](boost::intrusive_ptr< sisl::GenericRpcData >& rpc_data) {
        sisl::io_blob svr_buf;
        if (auto status = deserialize_from_byte_buffer(rpc_data->request(), svr_buf); !status.ok()) {
            LOGE(, "ByteBuffer DumpToSingleSlice failed, {}", status.error_message());
            rpc_data->set_status(status);
            return true; // respond immediately
        }
        handler(svr_buf, rpc_data);
        return false;
    };

    std::shared_lock lg{map_lock_};
    if (auto it = channel_map_.find(group_id); it != channel_map_.end()) {
        std::string const uniq_rpc_name = rpc_name + boost::uuids::to_string(group_id);

        auto const [rpc_id, already_added] = it->second->add_rpc(uniq_rpc_name, generic_handler_cb);
        if (already_added) {
            LOGW("Data service rpc {} exists for group_id={}", rpc_name, boost::uuids::to_string(group_id));
            return rpc_id;
        }

        if (rpc_id != invalid_rpc_id) {
            if (!grpc_server_->register_generic_rpc(uniq_rpc_name, generic_handler_cb)) {
                LOGE("Could not register generic rpc {} with gRPC! Not enough memory.", uniq_rpc_name);
                return invalid_rpc_id;
            }
        }
        return rpc_id;
    } else {
        LOGW("Unable to locate data channel for group_id={}", boost::uuids::to_string(group_id));
        return invalid_rpc_id;
    }
}
} // namespace nuraft_mesg