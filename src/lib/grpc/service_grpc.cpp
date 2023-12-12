#include "lib/grpc/service_grpc.hpp"

#include <sisl/grpc/rpc_server.hpp>
#include <sisl/logging/logging.h>
#include <sisl/options/options.h>

namespace nuraft_mesg {
DCSRaftServiceGrpc::DCSRaftServiceGrpc(shared< DCSManagerImpl > manager, DCSManager::Params const& params) :
        DCSRaftService(std::move(manager), params) {}

void DCSRaftServiceGrpc::start() override {
    auto listen_address = fmt::format(FMT_STRING("0.0.0.0:{}"), params_.mesg_port);
    LOGI("Starting Data Consensus Raft Service with GRPC on http://{}", listen_address);

    grpc_server_.reset();
    grpc_server_ = shared< sisl::GrpcServer >(sisl::GrpcServer::make(
        listen_address, params_.token_verifier, params_.rpc_server_threads, params_.ssl_key, params_.ssl_cert));

    grpc_server_->run();

    factory_ =
        std::make_shared< ClientFactoryGrpc >(params.rpc_client_threads, boost::uuids::to_string(params.server_uuid),
                                              manager_->application(), params.token_client, params.ssl_cert);
}

void DCSRaftServiceGrpc::shutdown() {
    grpc_server_->shutdown();
    DCSRaftService::shutdown();
}

DCSDataServiceGrpc::DCSDataServiceGrpc(shared< DCSManagerImpl > manager, DCSManager::Params const& params,
                                       shared< DCSRaftServiceGrpc > raft_service) :
        DCSDataService(std::move(manager), params) {
    if (raft_service) { grpc_server_ = raft_service->grpc_server(); }

    if (!grpc_server_) {
        auto listen_address = fmt::format(FMT_STRING("0.0.0.0:{}"), params_.mesg_port);
        LOGI("Starting Data Consensus Data Service with GRPC on http://{}", listen_address);

        grpc_server_ = shared< sisl::GrpcServer >(sisl::GrpcServer::make(
            listen_address, params_.token_verifier, params_.rpc_server_threads, params_.ssl_key, params_.ssl_cert));

        grpc_server_->run();
    } else {
        LOGI("Using Data Consensus Raft Service with GRPC for Data Service as well");
    }

    if (!grpc_server_->register_async_generic_service()) {
        throw std::runtime_error("Could not register generic service with gRPC!");
    }

    if (raft_service) { factory_ = raft_service->factory(); }
    if (!factory_) {
        factory_ = std::make_shared< ClientFactoryGrpc >(params.rpc_client_threads,
                                                         boost::uuids::to_string(params.server_uuid),
                                                         manager_->application(), params.token_client, params.ssl_cert);
    }

    start();
}

void DCSDataServiceGrpc::start() { rebind_requests(); }

void DCSDataServiceGrpc::rebind_requests() {
    auto lk = std::unique_lock< data_lock_type >(rpcs_mtx_);
    for (auto const& [request_name, cb] : _request_map) {
        if (!grpc_server_->register_generic_rpc(request_name, cb)) {
            throw std::runtime_error(fmt::format("Could not register generic rpc {} with gRPC!", request_name));
        }
    }
}

bool DCSDataServiceGrpc::bind_request(std::string const& request_name, group_id_t const& group_id,
                                      data_service_request_handler_t const& request_cb) {
    RELEASE_ASSERT(grpc_server_, "NULL grpc_server!");
    if (!request_cb) {
        LOGW("request_cb null for the request {}, cannot bind.", request_name);
        return false;
    }

    // This is an async call, hence the "return false". The user should invoke rpc_data->send_response to finish the
    // call
    auto generic_handler_cb = [request_cb](boost::intrusive_ptr< sisl::GenericRpcData >& rpc_data) {
        sisl::io_blob svr_buf;
        if (auto status = deserialize_from_byte_buffer(rpc_data->request(), svr_buf); !status.ok()) {
            LOGE(, "ByteBuffer DumpToSingleSlice failed, {}", status.error_message());
            rpc_data->set_status(status);
            return true; // respond immediately
        }
        request_cb(svr_buf, rpc_data);
        return false;
    };

    auto lk = std::unique_lock< data_lock_type >(req_lock_);
    auto [it, happened] = request_map_.emplace(get_generic_method_name(request_name, group_id), generic_handler_cb);
    if (it != request_map_.end()) {
        if (happened) {
            if (!grpc_server_->register_generic_rpc(it->first, it->second)) {
                throw std::runtime_error(fmt::format("Could not register generic rpc {} with gRPC!", request_name));
            }
        } else {
            LOGW(, "data service rpc {} exists", it->first);
            return false;
        }
    } else {
        throw std::runtime_error(
            fmt::format("Could not register generic rpc {} with gRPC! Not enough memory.", request_name));
    }
    return true;
}

unique< DataChannel > DCSDataServiceGrpc::create_data_channel() {
    return std::make_unique< DataChannelGrpc >(factory_, grpc_server_);
}
} // namespace nuraft_mesg