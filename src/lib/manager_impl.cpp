/// Copyright 2018 (c) eBay Corporation
//
#include <chrono>

#include <boost/uuid/string_generator.hpp>
#include <ios>
#include <spdlog/fmt/ostr.h>
#include <spdlog/details/registry.h>

#include <libnuraft/async.hxx>
#include <sisl/options/options.h>
#include <sisl/grpc/rpc_server.hpp>
#include <sisl/grpc/generic_service.hpp>

#include <nuraft_mesg/client_factory.hpp>
#include <nuraft_mesg/dcs_state_mgr.hpp>
#include <nuraft_mesg/nuraft_dcs.hpp>

#include "manager_impl.hpp"
// #include "repl_service_ctx.hpp"
#include "lib/service.hpp"
#include "lib/grpc/factory_grpc.hpp"
#include "lib/logger.hpp"

namespace nuraft_mesg {

int32_t to_server_id(peer_id_t const& server_addr) {
    boost::hash< boost::uuids::uuid > uuid_hasher;
    return uuid_hasher(server_addr) >> 33;
}

DCSManagerImpl::~DCSManagerImpl() {
    if (raft_service_) { raft_service_->shutdown(); }
    if (data_service_) { data_service_->shutdown(); }
}

DCSManagerImpl::DCSManagerImpl(DCSManager::Params const& start_params, weak< DCSApplication > app) :
        start_params_(start_params), srv_id_(to_server_id(start_params_.server_uuid)), application_(app) {
    auto logger_name = fmt::format("nuraft_{}", start_params_.server_uuid);
    //
    // NOTE: The Unit tests require this instance to be recreated with the same parameters.
    // This exception is only expected in this case where we "restart" the server by just recreating the instance.
    try {
        custom_logger_ = sisl::logging::CreateCustomLogger(logger_name, "", false, false /* tee_to_stdout_stderr */);
    } catch (spdlog::spdlog_ex const& e) { custom_logger_ = spdlog::details::registry::instance().get(logger_name); }

    sisl::logging::SetLogPattern("[%D %T.%f] [%^%L%$] [%t] %v", custom_logger_);
    nuraft::ptr< nuraft::logger > logger =
        std::make_shared< nuraft_mesg_logger >(start_params_.server_uuid, custom_logger_);

    // RAFT request scheduler
    nuraft::asio_service::options service_options;
    service_options.thread_pool_size_ = 1; // TODO: Is this translates to num commit threads?, should it be configured
    scheduler_ = std::make_shared< nuraft::asio_service >(service_options, logger);
}

void DCSManagerImpl::start() {
    auto lg = std::lock_guard< std::mutex >(manager_lock_);
    if (!raft_service_) {
        if (start_params_.raft_rpc_impl == DCSManager::Params::rpc_impl_t::grpc) {
            raft_service_ = std::make_shared< DCSRaftServiceGrpc >(shared_from_this(), start_params_);
        } else {
            RELEASE_ASSERT(false, "Only grpc implementation of Raft transport supported");
        }
    }
    raft_service_->start();

    if (start_params_.with_data_channel && !data_service_) {
        if ((start_params_.data_rpc_impl == DCSManager::Params::rpc_impl_t::grpc)) {
            data_service_ = std::make_shared< DCSDataServiceGrpc >(shared_from_this(), start_params_, raft_service_);
        } else {
            RELEASE_ASSERT(false, "Mixed rpc mode (for raft and data channels) is not supported yet");
        }
        data_service_->start();
    }
}

void DCSManagerImpl::restart_server() {
    std::lock_guard< std::mutex > lg(manager_lock_);
    RELEASE_ASSERT(raft_service_, "Need to call ::start() first!");
    raft_service_->start();
    if (start_params_.with_data_channel) { data_service_->start(); }
}

void DCSManagerImpl::register_mgr_type(group_type_t const& group_type, group_params const& params) {
    std::lock_guard< std::mutex > lg(manager_lock_);
    auto [it, happened] = state_mgr_types_.emplace(std::make_pair(group_type, params));
    DEBUG_ASSERT(state_mgr_types_.end() != it, "Out of memory?");
    DEBUG_ASSERT(!!happened, "Re-register?");
    if (state_mgr_types_.end() == it) { LOGE("Could not register [group_type={}]", group_type); }
}

void DCSManagerImpl::raft_event(group_id_t const& group_id, nuraft::cb_func::Type type, nuraft::cb_func::Param* param) {
    switch (type) {
    case nuraft::cb_func::RemovedFromCluster: {
        LOGI("[srv_id={}] evicted from: [group={}]", start_params_.server_uuid, group_id);
        exit_group(group_id);
    } break;
    case nuraft::cb_func::JoinedCluster: {
        auto const my_id = param->myId;
        auto const leader_id = param->leaderId;
        LOGI("[srv_id={}] joined: [group={}], [leader_id:{},my_id:{}]", start_params_.server_uuid, group_id, leader_id,
             my_id);
        {
            std::lock_guard< std::mutex > lg(manager_lock_);
            is_leader_[group_id] = (leader_id == my_id);
        }
    } break;
    case nuraft::cb_func::NewConfig: {
        LOGD("[srv_id={}] saw cluster change: [group={}]", start_params_.server_uuid, group_id);
        config_change_.notify_all();
    } break;
    case nuraft::cb_func::BecomeLeader: {
        LOGI("[srv_id={}] became leader: [group={}]!", start_params_.server_uuid, group_id);
        {
            std::lock_guard< std::mutex > lg(manager_lock_);
            is_leader_[group_id] = true;
        }
        config_change_.notify_all();
    } break;
    case nuraft::cb_func::BecomeFollower: {
        LOGI("[srv_id={}] following: [group={}]!", start_params_.server_uuid, group_id);
        {
            std::lock_guard< std::mutex > lg(manager_lock_);
            is_leader_[group_id] = false;
        }
    }
    default:
        break;
    };
}

void DCSManagerImpl::exit_group(group_id_t const& group_id) {
    shared< DCSStateManager > rg;
    {
        std::lock_guard< std::mutex > lg(manager_lock_);
        if (auto it = state_managers_.find(group_id); it != state_managers_.end()) { rg = it->second; }
    }
    if (rg) rg->leave();
}

nuraft::cmd_result_code DCSManagerImpl::group_init(int32_t const srv_id, group_id_t const& group_id,
                                                   group_type_t const& group_type, nuraft::context*& ctx) {
    LOGD("Creating context for: [group_id={}] as Member: {}", group_id, srv_id);

    // State manager (RAFT log store, config)
    shared< nuraft::state_mgr > smgr;
    shared< nuraft::state_machine > sm;
    nuraft::raft_params params;

    // Create Data channel if

    {
        std::lock_guard< std::mutex > lg(manager_lock_);
        auto def_group = state_mgr_types_.end();
        if (def_group = state_mgr_types_.find(group_type); state_mgr_types_.end() == def_group) {
            return nuraft::cmd_result_code::SERVER_NOT_FOUND;
        }
        params = def_group->second;

        auto [it, happened] = state_managers_.emplace(group_id, nullptr);
        if (it != state_managers_.end()) {
            if (happened) {
                // A new logstore!
                LOGD("Creating new State Manager for: [group={}], type: {}", group_id, group_type);
                it->second = application_.lock()->create_state_mgr(srv_id, group_id);
            }
            it->second->become_ready();
            sm = it->second->get_state_machine();
            smgr = it->second;
        } else {
            return nuraft::cmd_result_code::CANCELLED;
        }
    }

    // RAFT client factory
    unique< DataChannel > dc;
    if (start_params_.with_data_channel) { dc = std::move(data_service_->create_data_channel()); }

    shared< nuraft::rpc_client_factory > rpc_cli_factory(
        std::make_shared< GroupClientFactory >(raft_channel_factory_, group_id, group_type, dc ? dc->on_group_changed, nullptr));

    std::static_pointer_cast< DCSStateManager >(smgr)->set_data_channel(std::move(dc));

    // RAFT service interface (stops gRPC service etc...) (TODO)
    shared< nuraft::rpc_listener > listener;

    nuraft::ptr< nuraft::logger > logger = std::make_shared< nuraft_mesg_logger >(group_id, custom_logger_);
    ctx = new nuraft::context(smgr, sm, listener, logger, rpc_cli_factory, scheduler_, params);
    ctx->set_cb_func([wp = weak< DCSManagerImpl >(shared_from_this()), group_id](nuraft::cb_func::Type type,
                                                                                 nuraft::cb_func::Param* param) {
        if (auto sp = wp.lock(); sp) sp->raft_event(group_id, type, param);
        return nuraft::cb_func::Ok;
    });

    return nuraft::cmd_result_code::OK;
}

NullAsyncResult DCSManagerImpl::add_member(group_id_t const& group_id, peer_id_t const& new_id) {
    auto str_id = to_string(new_id);
    return raft_service_->add_member(group_id, nuraft::srv_config(to_server_id(new_id), str_id))
        .deferValue([this, g_id = group_id, n_id = std::move(str_id)](auto cmd_result) mutable -> NullResult {
            if (!cmd_result) return folly::makeUnexpected(cmd_result.error());
            // TODO This should not block, but attach a new promise!
            auto lk = std::unique_lock< std::mutex >(manager_lock_);
            if (!config_change_.wait_for(lk, start_params_.leader_change_timeout_ms,
                                         [this, g_id = std::move(g_id), n_id = std::move(n_id)]() {
                                             std::vector< shared< nuraft::srv_config > > srv_list;
                                             raft_service_->get_srv_config_all(g_id, srv_list);
                                             return std::find_if(srv_list.begin(), srv_list.end(),
                                                                 [n_id = std::move(n_id)](
                                                                     const shared< nuraft::srv_config >& cfg) {
                                                                     return n_id == cfg->get_endpoint();
                                                                 }) != srv_list.end();
                                         })) {
                return folly::makeUnexpected(nuraft::cmd_result_code::CANCELLED);
            }
            return folly::Unit();
        });
}

NullAsyncResult DCSManagerImpl::rem_member(group_id_t const& group_id, peer_id_t const& old_id) {
    return raft_service_->rem_member(group_id, to_server_id(old_id));
}

NullAsyncResult DCSManagerImpl::become_leader(group_id_t const& group_id) {
    {
        auto lk = std::unique_lock< std::mutex >(manager_lock_);
        if (is_leader_[group_id]) { return folly::Unit(); }
    }

    return folly::makeSemiFuture< folly::Unit >(folly::Unit())
        .deferValue([this, g_id = group_id](auto) mutable -> NullResult {
            if (!raft_service_->become_leader(g_id)) return folly::makeUnexpected(nuraft::cmd_result_code::CANCELLED);

            auto lk = std::unique_lock< std::mutex >(manager_lock_);
            if (!config_change_.wait_for(lk, start_params_.leader_change_timeout_ms,
                                         [this, g_id = std::move(g_id)]() { return is_leader_[g_id]; }))
                return folly::makeUnexpected(nuraft::cmd_result_code::TIMEOUT);
            return folly::Unit();
        });
}

NullAsyncResult DCSManagerImpl::append_entries(group_id_t const& group_id,
                                               std::vector< shared< nuraft::buffer > > const& buf) {
    return raft_service_->append_entries(group_id, buf);
}

shared< DCSStateManager > DCSManagerImpl::lookup_state_manager(group_id_t const& group_id) const {
    std::lock_guard< std::mutex > lg(manager_lock_);
    if (auto it = state_managers_.find(group_id); state_managers_.end() != it) return it->second;
    return nullptr;
}

NullAsyncResult DCSManagerImpl::create_group(group_id_t const& group_id, std::string const& group_type_name) {
    {
        std::lock_guard< std::mutex > lg(manager_lock_);
        is_leader_.insert(std::make_pair(group_id, false));
    }
    if (auto const err = raft_service_->joinRaftGroup(srv_id_, group_id, group_type_name); err) {
        return folly::makeUnexpected(err);
    }

    // Wait for the leader election timeout to make us the leader
    return folly::makeSemiFuture< folly::Unit >(folly::Unit())
        .deferValue([this, g_id = group_id](auto) mutable -> NullResult {
            auto lk = std::unique_lock< std::mutex >(manager_lock_);
            if (!config_change_.wait_for(lk, start_params_.leader_change_timeout_ms,
                                         [this, g_id = std::move(g_id)]() { return is_leader_[g_id]; })) {
                return folly::makeUnexpected(nuraft::cmd_result_code::CANCELLED);
            }
            return folly::Unit();
        });
}

NullResult DCSManagerImpl::join_group(group_id_t const& group_id, group_type_t const& group_type,
                                      shared< DCSStateManager > smgr) {
    {
        std::lock_guard< std::mutex > lg(manager_lock_);
        auto [it, happened] = state_managers_.emplace(group_id, smgr);
        if (state_managers_.end() == it) return folly::makeUnexpected(nuraft::cmd_result_code::CANCELLED);
    }
    if (auto const err = raft_service_->joinRaftGroup(srv_id_, group_id, group_type); err) {
        std::lock_guard< std::mutex > lg(manager_lock_);
        state_managers_.erase(group_id);
        return folly::makeUnexpected(err);
    }
    return folly::Unit();
}

void DCSManagerImpl::append_peers(group_id_t const& group_id, std::list< peer_id_t >& servers) const {
    auto it = state_managers_.end();
    {
        std::lock_guard< std::mutex > lg(manager_lock_);
        if (it = state_managers_.find(group_id); state_managers_.end() == it) return;
    }
    if (auto config = it->second->load_config(); config) {
        for (auto const& server : config->get_servers()) {
            servers.push_back(boost::uuids::string_generator()(server->get_endpoint()));
        }
    }
}

void DCSManagerImpl::leave_group(group_id_t const& group_id) {
    LOGI("Leaving group [group={}]", group_id);
    {
        std::lock_guard< std::mutex > lg(manager_lock_);
        if (0 == state_managers_.count(group_id)) {
            LOGD("Asked to leave [group={}] which we are not part of!", group_id);
            return;
        }
    }

    raft_service_->leave_group(group_id);

    std::lock_guard< std::mutex > lg(manager_lock_);
    if (auto it = state_managers_.find(group_id); state_managers_.end() != it) {
        // Delete all the state files (RAFT log etc.) after descrtuctor is called.
        it->second->permanent_destroy();
        state_managers_.erase(it);
    }

    LOGI("Finished leaving: [group={}]", group_id);
}

uint32_t DCSManagerImpl::logstore_id(group_id_t const& group_id) const {
    std::lock_guard< std::mutex > lg(manager_lock_);
    if (auto it = state_managers_.find(group_id); state_managers_.end() != it) { return it->second->get_logstore_id(); }
    return UINT32_MAX;
}

void DCSManagerImpl::get_srv_config_all(group_id_t const& group_id,
                                        std::vector< shared< nuraft::srv_config > >& configs_out) {
    raft_service_->get_srv_config_all(group_id, configs_out);
}

bool DCSManagerImpl::bind_data_channel_request(std::string const& request_name, group_id_t const& group_id,
                                               data_channel_request_handler_t const& request_handler) {
    RELEASE_ASSERT(data_service_, "Need to call ::start() first!");
    return data_service_->bind_data_channel_request(request_name, group_id, request_handler);
}

shared< DCSManager > init_dcs(DCSManager::Params const& p, weak< DCSApplication > w, bool with_data_svc) {
    RELEASE_ASSERT(w.lock(), "Could not acquire application!");
    auto m = std::make_shared< DCSManagerImpl >(p, w);
    m->start(with_data_svc);
    return m;
}

} // namespace nuraft_mesg
