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
//   Implements cornerstone's rpc_client::send(...) routine to translate
// and execute the call over gRPC asynchrously.
//
#pragma once

#include <libnuraft/nuraft.hxx>
#include <sisl/logging/logging.h>

#include "lib/common_lib.hpp"

namespace nuraft_mesg {

class dcs_raft_resp : public nuraft::resp_msg {
public:
    using nuraft::resp_msg::resp_msg;
    ~dcs_raft_resp() override = default;

    std::string dest_addr;
};

class DCSClient : public std::enable_shared_from_this< DCSClient > {
protected:
    static std::atomic_uint64_t s_client_counter;
    uint64_t client_id_;
    std::string const addr_;
    std::string const worker_name_;

public:
    DCSClient(std::string const& worker_name, std::string const& addr) :
            client_id_(s_client_counter++), addr_{addr}, worker_name_{worker_name} {}
    virtual ~DCSClient() = default;

    virtual bool is_connection_ready() const = 0;
    virtual bool is_service_ok() const = 0;
};

class GroupClient : public nuraft::rpc_client {
    static std::atomic_uint64_t s_client_counter;
    std::shared_ptr< DCSClient > dcs_client_;
    uint64_t client_id_;
    group_id_t const group_id_;
    group_type_t const group_type_;
    std::string const client_addr_;

public:
    GroupClient(std::shared_ptr< DCSClient > client, peer_id_t const& client_addr, group_id_t const& grp_id,
                group_type_t const& grp_type) :
            nuraft::rpc_client::rpc_client(),
            dcs_client_(std::move(client)),
            client_id_(s_client_counter++),
            group_id_(grp_id),
            group_type_(grp_type),
            client_addr_(to_string(client_addr)) {}

    ~GroupClient() override = default;

    bool is_abandoned() const override { return false; }
    uint64_t get_id() const override { return client_id_; }

    std::shared_ptr< DCSClient > realClient() { return dcs_client_; }
    void setClient(std::shared_ptr< DCSClient > new_client) { dcs_client_ = std::move(new_client); }
};

} // namespace nuraft_mesg
