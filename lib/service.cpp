///
// Copyright 2018 (c) eBay Corporation
//
// Authors:
//      Brian Szmyd <bszmyd@ebay.com>
//
// Brief:
//   Messaging service routines
//

#include <sds_options/options.h>

#include "grpcpp/impl/codegen/status_code_enum.h"
#include "libnuraft/async.hxx"
#include "libnuraft/rpc_listener.hxx"
#include "service.h"

SDS_LOGGING_DECL(sds_msg)

SDS_OPTION_GROUP(sds_messaging,
                 (messaging_metrics, "", "msg_metrics", "Gather metrics from SD Messaging", cxxopts::value< bool >(),
                  ""))

namespace sds::messaging {

using AsyncRaftSvc = Messaging::AsyncService;

grpc_server_wrapper::grpc_server_wrapper(group_name_t const& group_name) : m_server() {
    if (0 < SDS_OPTIONS.count("msg_metrics")) m_metrics = std::make_shared< group_metrics >(group_name);
}

shared< msg_service > msg_service::create(get_server_ctx_cb get_server_ctx, std::string const& service_address) {
    return std::shared_ptr< msg_service >(new msg_service(get_server_ctx, service_address),
                                          [](msg_service* p) { delete p; });
}

msg_service::~msg_service() {
    std::unique_lock< lock_type > lck(_raft_servers_lock);
    DEBUG_ASSERT(_raft_servers.empty(), "RAFT servers not fully terminated!");
}

void msg_service::associate(::grpc_helper::GrpcServer* server) {
    RELEASE_ASSERT(server, "NULL server!");
    if (!server->register_async_service< Messaging >()) {
        LOGERRORMOD(nuraft, "Could not register RaftSvc with gRPC!");
        abort();
    }
}

void msg_service::bind(::grpc_helper::GrpcServer* server) {
    RELEASE_ASSERT(server, "NULL server!");
    if (!server->register_rpc< Messaging, RaftGroupMsg, RaftGroupMsg, false >(
            "RaftStep", &AsyncRaftSvc::RequestRaftStep,
            std::bind(&msg_service::raftStep, this, std::placeholders::_1))) {
        LOGERRORMOD(nuraft, "Could not bind gRPC ::RaftStep to routine!");
        abort();
    }
}

nuraft::cmd_result_code msg_service::add_srv(group_name_t const& group_name, nuraft::srv_config const& cfg) {
    shared< nuraft_grpc::grpc_server > server;
    {
        std::shared_lock< lock_type > rl(_raft_servers_lock);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) { server = it->second.m_server; }
    }
    if (server) {
        try {
            return server->add_srv(cfg)->get_result_code();
        } catch (std::runtime_error& rte) { LOGERRORMOD(sds_msg, "Caught exception during add_srv(): {}", rte.what()); }
    }
    return nuraft::SERVER_NOT_FOUND;
}

nuraft::cmd_result_code msg_service::rm_srv(group_name_t const& group_name, int const member_id) {
    shared< nuraft_grpc::grpc_server > server;
    {
        std::shared_lock< lock_type > rl(_raft_servers_lock);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) { server = it->second.m_server; }
    }
    if (server) {
        try {
            return server->rem_srv(member_id)->get_result_code();
        } catch (std::runtime_error& rte) { LOGERRORMOD(sds_msg, "Caught exception during rm_srv(): {}", rte.what()); }
    }
    return nuraft::SERVER_NOT_FOUND;
}

bool msg_service::request_leadership(group_name_t const& group_name) {
    shared< nuraft_grpc::grpc_server > server;
    {
        std::shared_lock< lock_type > rl(_raft_servers_lock);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) { server = it->second.m_server; }
    }
    if (server) {
        try {
            return server->request_leadership();
        } catch (std::runtime_error& rte) {
            LOGERRORMOD(sds_msg, "Caught exception during yield_leadership(): {}", rte.what())
        }
    }
    return false;
}

nuraft::cmd_result_code msg_service::append_entries(group_name_t const& group_name,
                                                    std::vector< nuraft::ptr< nuraft::buffer > > const& logs) {
    shared< nuraft_grpc::grpc_server > server;
    {
        std::shared_lock< lock_type > rl(_raft_servers_lock);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) { server = it->second.m_server; }
    }
    if (server) {
        try {
            return server->append_entries(logs)->get_result_code();
        } catch (std::runtime_error& rte) { LOGERRORMOD(sds_msg, "Caught exception during step(): {}", rte.what()); }
    }
    return nuraft::SERVER_NOT_FOUND;
}

void msg_service::setDefaultGroupType(std::string const& _type) {
    std::shared_lock< lock_type > rl(_raft_servers_lock);
    _default_group_type = _type;
}

bool msg_service::raftStep(const grpc_helper::AsyncRpcDataPtr< Messaging, RaftGroupMsg, RaftGroupMsg >& rpc_data) {
    auto& request = rpc_data->request();
    auto& response = rpc_data->response();
    auto const& group_name = request.group_name();
    auto const& intended_addr = request.intended_addr();

    // Verify this is for the service it was intended for
    auto const& base = request.msg().base();
    if (intended_addr != _service_address) {
        LOGWARNMOD(sds_msg, "Recieved mesg for {} intended for {}, we are {}",
                   nuraft::msg_type_to_string(nuraft::msg_type(base.type())), intended_addr, _service_address);
        rpc_data->set_status(::grpc::Status(::grpc::INVALID_ARGUMENT, "Bad service address {}", intended_addr));
        return true;
    }

    LOGTRACEMOD(sds_msg, "Received [{}] from: [{}] to: [{}] Group: [{}]",
                nuraft::msg_type_to_string(nuraft::msg_type(base.type())), base.src(), base.dest(), group_name);

    if (nuraft::join_cluster_request == base.type()) { joinRaftGroup(base.dest(), group_name, request.group_type()); }

    shared< nuraft_grpc::grpc_server > server;
    {
        std::shared_lock< lock_type > rl(_raft_servers_lock);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) {
            if (it->second.m_metrics) COUNTER_INCREMENT(*it->second.m_metrics, group_steps, 1);
            server = it->second.m_server;
        }
    }

    response.set_group_name(group_name);
    if (server) {
        try {
            rpc_data->set_status(server->step(request.msg(), *response.mutable_msg()));
            return true;
        } catch (std::runtime_error& rte) { LOGERRORMOD(sds_msg, "Caught exception during step(): {}", rte.what()); }
    } else {
        LOGDEBUGMOD(sds_msg, "Missing RAFT group: {}", group_name);
    }
    rpc_data->set_status(::grpc::Status(::grpc::NOT_FOUND, "Missing RAFT group"));
    return true;
}

// We've hooked the gRPC server up to our "super-server", so we
// do not need to bind the grpc_servers to anything...just piggy-backing
// on their ::step() and transformations.
class null_service final : public nuraft_grpc::grpc_server {
public:
    using nuraft_grpc::grpc_server::grpc_server;
    void associate(grpc_helper::GrpcServer*) override{};
    void bind(grpc_helper::GrpcServer*) override{};
};

class msg_group_listner : public nuraft::rpc_listener {
    shared< msg_service > _svc;
    group_name_t _group;

public:
    msg_group_listner(shared< msg_service > svc, group_name_t const& group) : _svc(svc), _group(group) {}
    ~msg_group_listner() { _svc->shutdown_for(_group); }

    void listen(nuraft::ptr< nuraft::msg_handler >& handler) override {
        LOGINFOMOD(sds_msg, "Begin listening on {}", _group);
    }
    void stop() override { LOGINFOMOD(sds_msg, "Stop {}", _group); }
    void shutdown() override { LOGINFOMOD(sds_msg, "Shutdown {}", _group); }
};

void msg_service::shutdown_for(group_name_t const& group_name) {
    {
        std::unique_lock< lock_type > lck(_raft_servers_lock);
        LOGDEBUGMOD(sds_msg, "Shutting down RAFT group: {}", group_name);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) {
            _raft_servers.erase(it);
        } else {
            LOGWARNMOD(sds_msg, "Unknown RAFT group: {} cannot shutdown.", group_name);
            return;
        }
    }
    _raft_servers_sync.notify_all();
}

std::error_condition msg_service::joinRaftGroup(int32_t const srv_id, group_name_t const& group_name,
                                                group_type_t const& group_type) {
    LOGINFOMOD(sds_msg, "Joining RAFT group: {}, type: {}", group_name, group_type);

    nuraft::context* ctx{nullptr};
    bool happened{false};
    auto it = _raft_servers.end();
    {
        // This is for backwards compatibility for groups that had no type before.
        auto g_type = group_type;
        std::unique_lock< lock_type > lck(_raft_servers_lock);
        if (g_type.empty()) { g_type = _default_group_type; }
        std::tie(it, happened) = _raft_servers.emplace(std::make_pair(group_name, group_name));
        if (_raft_servers.end() != it && happened) {
            if (auto err = _get_server_ctx(srv_id, group_name, g_type, ctx, it->second.m_metrics, this); err) {
                LOGERRORMOD(sds_msg, "Error during RAFT server creation on group {}: {}", group_name, err.message());
                return err;
            }
            DEBUG_ASSERT(!ctx->rpc_listener_, "RPC listner should not be set!");
            auto new_listner = std::make_shared< msg_group_listner >(shared_from_this(), group_name);
            ctx->rpc_listener_ = std::static_pointer_cast< nuraft::rpc_listener >(new_listner);
            auto server = std::make_shared< nuraft::raft_server >(ctx);
            it->second.m_server = std::make_shared< null_service >(server);
        }
    }
    return std::error_condition();
}

void msg_service::partRaftGroup(group_name_t const& group_name) {
    shared< nuraft_grpc::grpc_server > server;

    {
        std::unique_lock< lock_type > lck(_raft_servers_lock);
        if (auto it = _raft_servers.find(group_name); _raft_servers.end() != it) {
            server = it->second.m_server;
        } else {
            LOGWARNMOD(sds_msg, "Unknown RAFT group: {} cannot part.", group_name);
            return;
        }
    }

    if (auto raft_server = server->raft_server(); raft_server) {
        LOGINFOMOD(sds_msg, "Parting RAFT group: {}", group_name);
        raft_server->stop_server();
        raft_server->shutdown();
    }
}

void msg_service::shutdown() {
    LOGINFOMOD(sds_msg, "MessagingService shutdown started.");
    std::deque< shared< nuraft_grpc::grpc_server > > servers;

    {
        std::unique_lock< lock_type > lck(_raft_servers_lock);
        for (auto& [k, v] : _raft_servers) {
            servers.push_back(v.m_server);
        }
    }

    for (auto& server : servers) {
        server->raft_server()->stop_server();
        server->raft_server()->shutdown();
    }

    {
        std::unique_lock< lock_type > lck(_raft_servers_lock);
        _raft_servers_sync.wait(lck, [this]() { return _raft_servers.empty(); });
    }
    LOGINFOMOD(sds_msg, "MessagingService shutdown complete.");
}

} // namespace sds::messaging
