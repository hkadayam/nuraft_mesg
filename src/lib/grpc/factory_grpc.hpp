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

#include <nuraft_mesg/client_factory.hpp>

namespace nuraft_mesg {
class ClientFactoryGrpc : public ClientFactory {
protected:
    std::shared_ptr< sisl::GrpcTokenClient > token_client;
    std::string ssl_cert; // TODO: Changed from static to member, should it be static???

public:
    ClientFactoryGrpc(uint32_t cli_thread_count, std::string const& name, weak< DCSApplication >& app,
                      shared< sisl::GrpcTokenClient > token_client, std::string const& ssl_cert);
    virtual ~ClientFactoryGrpc() = default;

protected:
    nuraft::cmd_result_code _create_raft_client(peer_id_t const& client,
                                                nuraft::ptr< nuraft::rpc_client >& raft_client) override;
};
} // namespace nuraft_mesg