#include "service.hpp"

#include <boost/functional/hash.hpp>
#include <grpcpp/impl/codegen/status_code_enum.h>
#include <libnuraft/async.hxx>
#include <libnuraft/rpc_listener.hxx>
#include <sisl/options/options.h>

#include <nuraft_mesg/client_factory.hpp>
#include <nuraft_mesg/dcs_state_mgr.hpp>
#include <nuraft_mesg/nuraft_dcs.hpp>

#define CONTINUE_RESP(resp)                                                                                            \
    try {                                                                                                              \
        if (auto r = (resp)->get_result_code(); r != nuraft::RESULT_NOT_EXIST_YET) {                                   \
            if (nuraft::OK == r) return folly::Unit();                                                                 \
            return folly::makeUnexpected(r);                                                                           \
        }                                                                                                              \
        auto [p, sf] = folly::makePromiseContract< NullResult >();                                                     \
        (resp)->when_ready(                                                                                            \
            [p = std::make_shared< decltype(p) >(std::move(p))](                                                       \
                nuraft::cmd_result< nuraft::ptr< nuraft::buffer >, nuraft::ptr< std::exception > >& result,            \
                auto&) mutable {                                                                                       \
                if (nuraft::cmd_result_code::OK != result.get_result_code())                                           \
                    p->setValue(folly::makeUnexpected(result.get_result_code()));                                      \
                else                                                                                                   \
                    p->setValue(folly::Unit());                                                                        \
            });                                                                                                        \
        return std::move(sf);                                                                                          \
    } catch (std::runtime_error & rte) { LOGE("Caught exception: [group={}] {}", group_id, rte.what()); }

namespace nuraft_mesg {

DCSRaftService::DCSRaftService(shared< DCSManagerImpl > const& manager, DCSManager::Params const& params) :
        default_group_type(params.default_group_type),
        params_{params},
        manager_(manager),
        service_address_(params.server_uuid) {}

DCSRaftService::~DCSRaftService() = default;

NullAsyncResult DCSRaftService::add_member(group_id_t const& group_id, nuraft::srv_config const& cfg) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) { CONTINUE_RESP(it->second->add_srv(cfg)) }
    return folly::makeUnexpected(nuraft::SERVER_NOT_FOUND);
}

NullAsyncResult DCSRaftService::rem_member(group_id_t const& group_id, int const member_id) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) {
        CONTINUE_RESP(it->second->rem_srv(member_id))
    }
    return folly::makeUnexpected(nuraft::SERVER_NOT_FOUND);
}

bool DCSRaftService::become_leader(group_id_t const& group_id) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) {
        try {
            return it->second->request_leadership();
        } catch (std::runtime_error& rte) { LOGE("Caught exception during request_leadership(): {}", rte.what()) }
    }
    LOGW("Unknown [group={}] cannot get config.", group_id);
    return false;
}

void DCSRaftService::get_srv_config_all(group_id_t const& group_id,
                                        std::vector< shared< nuraft::srv_config > >& configs_out) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) {
        try {
            it->second->get_srv_config_all(configs_out);
        } catch (std::runtime_error& rte) { LOGE("Caught exception during add_srv(): {}", rte.what()); }
    } else {
        LOGW("Unknown [group={}] cannot get config.", group_id);
    }
}

NullAsyncResult DCSRaftService::append_entries(group_id_t const& group_id,
                                               std::vector< nuraft::ptr< nuraft::buffer > > const& logs) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) {
        CONTINUE_RESP(it->second->append_entries(logs))
    }
    return folly::makeUnexpected(nuraft::SERVER_NOT_FOUND);
}

class RaftGroupListener : public nuraft::rpc_listener {
    std::weak_ptr< DCSRaftService > svc_;
    group_id_t group_;

public:
    RaftGroupListener(shared< DCSRaftService > const& svc, group_id_t const& group) : svc_(svc), group_(group) {}
    ~RaftGroupListener() {
        if (auto svc = svc_.lock(); svc) svc->shutdown_for(group_);
    }

    void listen(nuraft::ptr< nuraft::msg_handler >&) override { LOGI("[group={}]", group_); }
    void stop() override { LOGI("[group={}]", group_); }
    void shutdown() override { LOGI("[group={}]", group_); }
};

void DCSRaftService::shutdown_for(group_id_t const& group_id) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) {
        LOGD("Shutting down [group={}]", group_id);
        raft_servers_.erase(it);
    } else {
        LOGW("Unknown [group={}] cannot shutdown.", group_id);
    }
}

nuraft::cmd_result_code DCSRaftService::joinRaftGroup(int32_t const srv_id, group_id_t const& group_id,
                                                      group_type_t const& group_type) {
    LOGI("Joining RAFT [group={}], type: {}", group_id, group_type);
    auto mgr = manager_.lock();
    if (!mgr) {
        LOGW("Got join after shutdown...skipping [group={}]", group_id);
        return nuraft::cmd_result_code::CANCELLED;
    }

    // This is for backwards compatibility for groups that had no type before.
    auto const g_type = group_type.empty() ? default_group_type : group_type;
    // Quick check for duplicate, this will not guarantee we do not instantiate
    // more than one state_mgr, but it will quickly be destroyed
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) return nuraft::cmd_result_code::OK;

    nuraft::context* ctx{nullptr};
    if (auto err = mgr->group_init(srv_id, group_id, g_type, ctx); err) {
        LOGE("Error during RAFT server creation [group={}]: {}", group_id, err);
        return err;
    }

    DEBUG_ASSERT(!ctx->rpc_listener_, "RPC listner should not be set!");
    auto new_listner = std::make_shared< RaftGroupListener >(shared_from_this(), group_id);
    ctx->rpc_listener_ = std::static_pointer_cast< nuraft::rpc_listener >(new_listner);

    auto server = std::make_shared< nuraft::raft_server >(ctx);
    auto [it, happened] = raft_servers_.try_emplace(group_id, std::make_unique< RaftGroupServer >(server, group_id));
    if (!happened) { RELEASE_ASSERT(raft_servers_.end() != it, "FAILED to add a new raft server!"); }
    return nuraft::cmd_result_code::OK;
}

void DCSRaftService::leave_group(group_id_t const& group_id) {
    if (auto it = raft_servers_.find(group_id); raft_servers_.end() != it) {
        it->second->raft_server()->stop_server();
        it->second->raft_server()->shutdown();
    } else {
        LOGW("Unknown [group={}] cannot part.", group_id);
    }
}

void DCSRaftService::shutdown() {
    LOGI("Data Consesus Service shutdown started.");

    for (auto& [_, v] : raft_servers_) {
        v->raft_server()->stop_server();
        v->raft_server()->shutdown();
    }
}
} // namespace nuraft_mesg
