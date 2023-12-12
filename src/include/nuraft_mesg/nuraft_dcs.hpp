/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <libnuraft/raft_params.hxx>

#include <nuraft_mesg/common.hpp>

namespace grpc {
class ByteBuffer;
class Status;
} // namespace grpc

namespace nuraft {
class buffer;
class srv_config;
} // namespace nuraft

namespace sisl {
class GenericRpcData;
class GrpcTokenVerifier;
class GrpcTokenClient;
} // namespace sisl

namespace nuraft_mesg {

class DCSStateManager;

// called by the server after it receives the request
using data_channel_request_handler_t = sisl::generic_rpc_handler_cb_t;

class DCSApplication {
public:
    virtual ~DCSApplication() = default;
    virtual std::string lookup_peer(peer_id_t const&) = 0;
    virtual shared< DCSStateManager > create_state_mgr(int32_t const srv_id, group_id_t const& group_id) = 0;
};

class DCSManager {
public:
    struct Params {
        enum rpc_impl_t : uint8_t { grpc, asio };

        boost::uuids::uuid server_uuid;
        uint16_t mesg_port;
        group_type_t default_group_type;
        std::string ssl_key;
        std::string ssl_cert;
        shared< sisl::GrpcTokenVerifier > token_verifier{nullptr};
        shared< sisl::GrpcTokenClient > token_client{nullptr};
        std::chrono::milliseconds leader_change_timeout_ms{3200};
        uint32_t rpc_client_threads{1};
        uint32_t rpc_server_threads{1};
        rpc_impl_t raft_rpc_impl{rpc_impl_t::grpc};
        rpc_impl_t data_rpc_impl{rpc_impl_t::grpc};
        bool with_data_channel{false};
    };

    using group_params = nuraft::raft_params;
    virtual ~DCSManager() = default;

    // Register a new group type
    virtual void register_mgr_type(group_type_t const& group_type, group_params const&) = 0;

    virtual shared< DCSStateManager > lookup_state_manager(group_id_t const& group_id) const = 0;
    virtual NullAsyncResult create_group(group_id_t const& group_id, group_type_t const& group_type) = 0;
    virtual NullResult join_group(group_id_t const& group_id, group_type_t const& group_type,
                                  shared< DCSStateManager >) = 0;

    // Send a client request to the cluster
    virtual NullAsyncResult add_member(group_id_t const& group_id, peer_id_t const& server_id) = 0;
    virtual NullAsyncResult rem_member(group_id_t const& group_id, peer_id_t const& server_id) = 0;
    virtual NullAsyncResult become_leader(group_id_t const& group_id) = 0;
    virtual NullAsyncResult append_entries(group_id_t const& group_id,
                                           std::vector< shared< nuraft::buffer > > const&) = 0;

    // Misc Mgmt
    virtual void get_srv_config_all(group_id_t const& group_id,
                                    std::vector< shared< nuraft::srv_config > >& configs_out) = 0;
    virtual void leave_group(group_id_t const& group_id) = 0;
    virtual void append_peers(group_id_t const& group_id, std::list< peer_id_t >&) const = 0;
    virtual uint32_t logstore_id(group_id_t const& group_id) const = 0;
    virtual int32_t server_id() const = 0;
    virtual void restart_server() = 0;

    // data channel APIs
    virtual bool bind_data_channel_request(std::string const& request_name, group_id_t const& group_id,
                                           data_channel_request_handler_t const&) = 0;
};

extern int32_t to_server_id(peer_id_t const& server_addr);

extern shared< DCSManager > init_dcs(DCSManager::Params const&, std::weak_ptr< DCSApplication >,
                                     bool with_data_svc = false);

} // namespace nuraft_mesg
