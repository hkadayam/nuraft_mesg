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

#include <memory>
#include <string>
#include <map>

#include <folly/SharedMutex.h>
#include <sisl/logging/logging.h>
#include <libnuraft/rpc_cli_factory.hxx>
#include <libnuraft/srv_config.hxx>

#include <nuraft_mesg/common.hpp>

namespace nuraft_mesg {

using client_factory_lock_type = folly::SharedMutex;
class DCSApplication;
class DCSClient;

// Class to track a common ClientFactory one per application and it provides a common mechanism for multiple raft
// groups to create clients. This class also acts a registry of all per group client factory created per peer.
class ClientFactory {
protected:
    std::string worker_name_;
    std::weak_ptr< DCSApplication > application_;

    client_factory_lock_type client_lock_;
    std::map< peer_id_t, std::shared_ptr< DCSClient > > dcs_clients_;

public:
    ClientFactory(std::string const& name, std::weak_ptr< DCSApplication >& app) :
            worker_name_{name}, application_{app} {}

    std::string const& worker_name() const { return worker_name_; }
    virtual shared< DCSClient > create_get_client(std::string const& client);
    virtual shared< DCSClient > create_get_client(peer_id_t const& client);
    virtual nuraft::cmd_result_code create_get_client(peer_id_t const& client, shared< DCSClient >&);

    virtual nuraft::cmd_result_code reinit_client(peer_id_t const& client, shared< DCSClient >&);

    // Construct and send an AddServer message to the cluster
    virtual NullAsyncResult add_server(uint32_t srv_id, peer_id_t const& srv_addr, nuraft::srv_config const& dest_cfg);

    // Send a client request to the cluster
    virtual NullAsyncResult append_entry(std::shared_ptr< nuraft::buffer > buf, nuraft::srv_config const& dest_cfg);

    // Construct and send a RemoveServer message to the cluster
    virtual NullAsyncResult rem_server(uint32_t srv_id, nuraft::srv_config const& dest_cfg);

protected:
    virtual nuraft::cmd_result_code _create_client(peer_id_t const& client, shared< DCSClient >& dcs_client) = 0;
};

using group_change_cb_t = std::function< void(peer_id_t const&, shared< DCSClient >) >;

// Class we supply to the RAFT for it to call create_client when Raft requires to do so. This class is instantiated for
// one per RAFT group.
class GroupClientFactory : public nuraft::rpc_client_factory,
                           public std::enable_shared_from_this< GroupClientFactory > {
    shared< ClientFactory > common_factory_;
    group_id_t const group_id_;
    group_type_t const group_type_;
    group_change_cb_t group_change_cb_;

public:
    GroupClientFactory(shared< ClientFactory > common_factory, group_id_t const& grp_id, group_type_t const& grp_type,
                       group_change_cb_t cb) :
            rpc_client_factory{},
            common_factory_{common_factory},
            group_id_(grp_id),
            group_type_(grp_type),
            group_change_cb_{cb} {}

    group_id_t group_id() const { return group_id_; }

    nuraft::ptr< nuraft::rpc_client > create_client(const std::string& endpoint) override;
    nuraft::cmd_result_code create_client(peer_id_t const& client, nuraft::ptr< nuraft::rpc_client >& rpc_ptr);
    nuraft::cmd_result_code reinit_client(peer_id_t const& client, std::shared_ptr< nuraft::rpc_client >& raft_client);
};
} // namespace nuraft_mesg
