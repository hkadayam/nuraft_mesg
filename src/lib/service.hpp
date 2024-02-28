///
// Copyright 2018 (c) eBay Corporation
//
#pragma once

#include <map>

#include <libnuraft/async.hxx>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <sisl/grpc/rpc_server.hpp>
#include <sisl/metrics/metrics.hpp>

#include <nuraft_mesg/dcs_state_mgr.hpp>
#include "group_server.hpp"
#include "manager_impl.hpp"

/// This is for the ConcurrentHashMap
/*namespace std {
template <>
struct hash< boost::uuids::uuid > {
    size_t operator()(const boost::uuids::uuid& uid) const noexcept { return boost::hash< boost::uuids::uuid >()(uid); }
};
} // namespace std
*/
namespace nuraft_mesg {

struct uuid_hasher {
    size_t operator()(const boost::uuids::uuid& uid) const { return boost::hash< boost::uuids::uuid >()(uid); }
};

class DCSRaftService : public nuraft::raft_server_handler, public std::enable_shared_from_this< DCSRaftService > {
protected:
    std::string const default_group_type;
    DCSManager::Params params_;
    std::weak_ptr< DCSManagerImpl > manager_;
    folly::ConcurrentHashMap< group_id_t, unique< RaftGroupServer >, uuid_hasher > raft_servers_;
    peer_id_t const service_address_;
    shared< ClientFactory > factory_;

public:
    DCSRaftService(shared< DCSManagerImpl > const& manager, DCSManager::Params const& params);
    virtual ~DCSRaftService();
    DCSRaftService(DCSRaftService const&) = delete;
    DCSRaftService& operator=(DCSRaftService const&) = delete;

    // Override the following for each serialization implementation
    virtual void start() = 0;
    virtual void shutdown();

    NullAsyncResult add_member(group_id_t const& group_id, nuraft::srv_config const& cfg);
    NullAsyncResult rem_member(group_id_t const& group_id, int const member_id);
    bool become_leader(group_id_t const& group_id);
    NullAsyncResult append_entries(group_id_t const& group_id,
                                   std::vector< nuraft::ptr< nuraft::buffer > > const& logs);

    void get_srv_config_all(group_id_t const& group_id, std::vector< shared< nuraft::srv_config > >& configs_out);
    void leave_group(group_id_t const& group_id);

    nuraft::cmd_result_code joinRaftGroup(int32_t srv_id, group_id_t const& group_id, group_type_t const&);

    // Internal intent only
    void shutdown_for(group_id_t const&);

    shared< ClientFactory > client_factory() { return factory_; }
};

class DCSDataService {
protected:
    shared< ClientFactory > factory_;

public:
    DCSDataService() = default;
    virtual void start() = 0;
    virtual void shutdown() = 0;
    virtual shared< DataChannel > create_data_channel(channel_id_t const& channel_id) = 0;
    virtual void remove_data_channel(channel_id_t const& channel_id) = 0;
    virtual rpc_id_t register_rpc(std::string const& rpc_name, group_id_t const& group_id,
                                  data_channel_rpc_handler_t const& handler) = 0;

    shared< ClientFactory > client_factory() { return factory_; }
};
} // namespace nuraft_mesg
