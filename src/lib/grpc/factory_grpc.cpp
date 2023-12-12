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
#include <boost/uuid/string_generator.hpp>
#include <libnuraft/async.hxx>

#include <nuraft_mesg/client_factory.hpp>

#include "lib/client.hpp"
#include "lib/grpc/factory_grpc.hpp"

namespace nuraft_mesg {

ClientFactoryGrpc::ClientFactoryGrpc(uint32_t cli_thread_count, std::string const& name, weak< DCSApplication >& app,
                                     shared< sisl::GrpcTokenClient > const token_client, std::string const& ssl_cert) :
        ClientFactory(name, app), token_client{std::move(token_client)}, ssl_cert{ssl_cert} {
    if (cli_thread_count > 0) { sisl::GrpcAsyncClientWorker::create_worker(name.data(), cli_thread_count); }
}

nuraft::cmd_result_code ClientFactoryGrpc::_create_client(peer_id_t const& client,
                                                          nuraft::ptr< nuraft::rpc_client >& raft_client) {
    LOGD("Creating client to {}", client);
    auto endpoint = application_.lock()->lookupEndpoint(client);
    if (endpoint.empty()) return nuraft::BAD_REQUEST;

    LOGD("Creating client for [{}] @ [{}]", client, endpoint);
    raft_client = sisl::GrpcAsyncClient::make< DCSClientGrpc >(worker_name(), endpoint, token_client, "", ssl_cert);
    return (!raft_client) ? nuraft::CANCELLED : nuraft::OK;
}

} // namespace nuraft_mesg
