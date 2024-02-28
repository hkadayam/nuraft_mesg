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

#include "lib/client.hpp"
#include "lib/grpc/client_grpc.hpp"

namespace nuraft_mesg {
class RaftGroupMsg;

using handle_resp = std::function< void(RaftMessage&, ::grpc::Status&) >;

class DCSClientProtob : public DCSClientGrpc, public std::enable_shared_from_this< DCSClientProtob > {
public:
    DCSClientProtob(std::string const& worker_name, std::string const& addr,
                    const std::shared_ptr< sisl::GrpcTokenClient > token_client, std::string const& target_domain = "",
                    std::string const& ssl_cert = "") :
            DCSClientGrpc(worker_name, addr, token_client, target_domain, ssl_cert) {}
    ~DCSClientProtob() override = default;

    std::atomic_uint bad_service;

    void send(RaftGroupMsg const& message, handle_resp complete);

    bool is_service_ok() const override { return (bad_service.load(std::memory_order_relaxed) == 0); }
};

class GroupClientProtob : public GroupClient {
public:
    GroupClientProtob(std::shared_ptr< DCSClient > dcs_client, peer_id_t const& client_addr, group_id_t const& grp_name,
                      group_type_t const& grp_type) :
            GroupClient(std::move(dcs_client), client_addr, grp_name, grp_type) {}

    ~GroupClientProtob() override = default;

    ///
    // This is where the magic of serialization happens starting with creating a RaftMessage and invoking our
    // specific ::send() which will later transform into a RaftGroupMsg
    void send(std::shared_ptr< nuraft::req_msg >& req, nuraft::rpc_handler& complete, uint64_t) override;

private:
    void send(RaftMessage const& message, handle_resp complete);
};
} // namespace nuraft_mesg