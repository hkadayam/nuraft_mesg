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

// Brief:
//   RaftGroupServer step function and response transformations
//
#include "group_server.hpp"

namespace nuraft_mesg {

nuraft::ptr< nuraft::cmd_result< nuraft::ptr< nuraft::buffer > > >
RaftGroupServer::add_srv(const nuraft::srv_config& cfg) {
    return raft_server_->add_srv(cfg);
}

void RaftGroupServer::yield_leadership(bool immediate) { return raft_server_->yield_leadership(immediate, -1); }

bool RaftGroupServer::request_leadership() { return raft_server_->request_leadership(); }

void RaftGroupServer::get_srv_config_all(std::vector< nuraft::ptr< nuraft::srv_config > >& configs_out) {
    raft_server_->get_srv_config_all(configs_out);
}

nuraft::ptr< nuraft::cmd_result< nuraft::ptr< nuraft::buffer > > > RaftGroupServer::rem_srv(int const member_id) {
    return raft_server_->remove_srv(member_id);
}

nuraft::ptr< nuraft::cmd_result< nuraft::ptr< nuraft::buffer > > >
RaftGroupServer::append_entries(std::vector< nuraft::ptr< nuraft::buffer > > const& logs) {
    return raft_server_->append_entries(logs);
}

nuraft::raft_server* DCSStateManager::raft_server() { return group_server_->raft_server().get(); }

void DCSStateManager::set_raft_group_server(shared< RaftGroupServer > group_server) {
    group_server_ = std::move(group_server);
}

bool DCSStateManager::is_raft_leader() const { group_server_->is_leader(); }

} // namespace nuraft_mesg
