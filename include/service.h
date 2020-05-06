///
// Copyright 2018 (c) eBay Corporation
//

#pragma once

#include <map>
#include <shared_mutex>
#include <nuraft_grpc/grpc_server.hpp>
#include <metrics/metrics.hpp>

#include "messaging_service.grpc.pb.h"

namespace sds::messaging {

template < typename T >
using boxed = std::unique_ptr< T >;

template < typename T >
using shared = std::shared_ptr< T >;

using group_name_t = std::string;

class msg_service;

using lock_type = std::shared_mutex;

class group_metrics : public sisl::MetricsGroupWrapper {
public:
    explicit group_metrics(group_name_t const& group_name) :
            sisl::MetricsGroupWrapper("RAFTGroup", group_name.c_str()) {
        REGISTER_COUNTER(group_steps, "Total group messages received", "raft_group", {"op", "step"});
        REGISTER_COUNTER(group_sends, "Total group messages sent", "raft_group", {"op", "send"});
        register_me_to_farm();
    }
};

using get_server_ctx_cb =
    std::function< std::error_condition(int32_t srv_id, group_name_t const&, nuraft::context*& ctx_out,
                                        shared< group_metrics > metrics, msg_service* sds_msg) >;

struct grpc_server_wrapper {
    explicit grpc_server_wrapper(group_name_t const& group_name) :
            m_server(), m_metrics(std::make_shared< group_metrics >(group_name)) {}

    shared< sds::grpc_server > m_server;
    shared< group_metrics >    m_metrics;
};

class msg_service {
    get_server_ctx_cb                             _get_server_ctx;
    lock_type                                     _raft_servers_lock;
    std::map< group_name_t, grpc_server_wrapper > _raft_servers;

public:
    explicit msg_service(get_server_ctx_cb get_server_ctx) : _get_server_ctx(get_server_ctx) {}
    ~msg_service();
    msg_service(msg_service const&) = delete;
    msg_service& operator=(msg_service const&) = delete;

    nuraft::cmd_result_code append_entries(group_name_t const&                                 group_name,
                                           std::vector< nuraft::ptr< nuraft::buffer > > const& logs);

    void associate(sds::grpc::GrpcServer* server);
    void bind(sds::grpc::GrpcServer* server);

    ::grpc::Status raftStep(RaftGroupMsg& request, RaftGroupMsg& response);

    std::error_condition createRaftGroup(group_name_t const& group_name) { return joinRaftGroup(0, group_name); }

    std::error_condition joinRaftGroup(int32_t srv_id, group_name_t const& group_name);

    void partRaftGroup(group_name_t const& group_name);
};

} // namespace sds::messaging
