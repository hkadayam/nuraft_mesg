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

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>

#include <sisl/logging/logging.h>
#include <libnuraft/nuraft.hxx>

#include <nuraft_mesg/nuraft_dcs.hpp>
#include <nuraft_mesg/client_factory.hpp>
#include "common_lib.hpp"

namespace nuraft_mesg {
class DCSService;

class DCSManagerImpl : public DCSManager, public std::enable_shared_from_this< DCSManagerImpl > {
    DCSManager::Params start_params_;
    int32_t srv_id_;

    std::map< group_type_t, DCSManager::group_params > state_mgr_types_;

    weak< DCSApplication > application_;
    shared< ClientFactory > data_channel_factory_;

    // Protected
    std::mutex mutable manager_lock_;
    shared< DCSRaftService > raft_service_;
    shared< DCSDataService > data_service_;
    std::map< group_id_t, shared< DCSStateManager > > state_managers_;
    std::condition_variable config_change_;
    std::map< group_id_t, bool > is_leader_;
    //

    nuraft::ptr< nuraft::delayed_task_scheduler > scheduler_;
    shared< sisl::logging::logger_t > custom_logger_;

    void raft_event(group_id_t const& group_id, nuraft::cb_func::Type type, nuraft::cb_func::Param* param);
    void exit_group(group_id_t const& group_id);

public:
    DCSManagerImpl(DCSManager::Params const&, weak< DCSApplication >);
    ~DCSManagerImpl() override;

    // Public API
    void register_mgr_type(group_type_t const& group_type, group_params const&) override;

    shared< DCSStateManager > lookup_state_manager(group_id_t const& group_id) const override;
    NullAsyncResult create_group(group_id_t const& group_id, group_type_t const& group_type) override;
    NullResult join_group(group_id_t const& group_id, group_type_t const& group_type,
                          shared< DCSStateManager > smgr) override;

    NullAsyncResult add_member(group_id_t const& group_id, peer_id_t const& server_id) override;
    NullAsyncResult rem_member(group_id_t const& group_id, peer_id_t const& server_id) override;
    NullAsyncResult become_leader(group_id_t const& group_id) override;
    NullAsyncResult append_entries(group_id_t const& group_id, std::vector< shared< nuraft::buffer > > const&) override;

    void get_srv_config_all(group_id_t const& group_id,
                            std::vector< shared< nuraft::srv_config > >& configs_out) override;
    void leave_group(group_id_t const& group_id) override;
    void append_peers(group_id_t const& group_id, std::list< peer_id_t >&) const override;
    uint32_t logstore_id(group_id_t const& group_id) const override;
    int32_t server_id() const override { return srv_id_; }
    void restart_server() override;

    bool bind_data_channel_request(std::string const& request_name, group_id_t const& group_id,
                                   data_channel_request_handler_t const& request_handler) override;
    //

    /// Internal API
    nuraft::cmd_result_code group_init(int32_t const srv_id, group_id_t const& group_id, group_type_t const& group_type,
                                       bool with_data_channel, nuraft::context*& ctx);
    void start(bool and_data_svc);
    weak< DCSApplication > application() { return application_; }
    //
};

} // namespace nuraft_mesg
