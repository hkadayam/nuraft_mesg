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
#include <sisl/grpc/rpc_client.hpp>
#include <nuraft_mesg/nuraft_dcs.hpp>
#include <nuraft_mesg/client_factory.hpp>

#include "lib/client.hpp"
#include "lib/grpc/client_grpc.hpp"
#include "lib/grpc/factory_grpc.hpp"
#include "lib/grpc/proto/client_proto.hpp"

namespace nuraft_mesg {

ClientFactoryGrpc::ClientFactoryGrpc(uint32_t cli_thread_count, std::string const& name, weak< DCSApplication > app,
                                     shared< sisl::GrpcTokenClient > const token_client, std::string const& ssl_cert) :
        ClientFactory(name, app), token_client{std::move(token_client)}, ssl_cert{ssl_cert} {
    if (cli_thread_count > 0) { sisl::GrpcAsyncClientWorker::create_worker(name.data(), cli_thread_count); }
}

nuraft::cmd_result_code ClientFactoryGrpc::_create_client(peer_id_t const& client, shared< DCSClient >& dcs_client) {
    LOGD("Creating client to {}", client);
    auto endpoint = application_.lock()->lookup_peer(client);
    if (endpoint.empty()) return nuraft::BAD_REQUEST;

    LOGD("Creating client for [{}] @ [{}]", client, endpoint);
    dcs_client = sisl::GrpcAsyncClient::make< DCSClientProtob >(worker_name(), endpoint, token_client, "", ssl_cert);
    return (!dcs_client) ? nuraft::CANCELLED : nuraft::OK;
}

nuraft::cmd_result_code ClientFactoryGrpc::_reinit_client(peer_id_t const& client_id, shared< DCSClient >& dcs_client) {
    LOGD("Re-init client_id={}", client_id);
    assert(dcs_client);
    if (!dcs_client->is_connection_ready() || !dcs_client->is_service_ok()) {
        return _create_client(client_id, dcs_client);
    }
    return nuraft::OK;
}

nuraft::ptr< nuraft::rpc_client > GroupClientFactoryGrpc::create_client(std::string const& client_id) {
    nuraft::ptr< nuraft::rpc_client > client;
    auto code = create_client(boost::uuids::string_generator()(client_id), client);
    if (code != nuraft::OK) { LOGC("Client Endpoint Invalid! [{}], error={}", client_id, code); }
    return client;
}

nuraft::cmd_result_code GroupClientFactoryGrpc::create_client(peer_id_t const& client_id,
                                                              nuraft::ptr< nuraft::rpc_client >& raft_client) {
    // Re-direct this call to a global factory so we can re-use clients to the same endpoints
    LOGD("Creating/Getting client to peer {}", client_id);
    auto nexus_client = common_factory_->create_get_client(client_id);
    if (!nexus_client) return nuraft::CANCELLED;

    // Create a raft group specific client by attaching the nexus_client
    raft_client = std::make_shared< GroupClientProtob >(nexus_client, client_id, group_id_, group_type_);
    if (!raft_client) { return nuraft::BAD_REQUEST; }

    if (group_change_cb_) { group_change_cb_(client_id, nexus_client); }
    return nuraft::OK;
}

nuraft::cmd_result_code GroupClientFactoryGrpc::reinit_client(peer_id_t const& client_id,
                                                              std::shared_ptr< nuraft::rpc_client >& raft_client) {
    LOGD("Re-init client to peer {}", client_id);
    auto grp_client = std::dynamic_pointer_cast< GroupClient >(raft_client);

    shared< DCSClient > new_client = grp_client->realClient();
    if (auto err = common_factory_->reinit_client(client_id, new_client); err) { return err; }
    grp_client->setClient(new_client);

    if (group_change_cb_) { group_change_cb_(client_id, new_client); }
    return nuraft::OK;
}
} // namespace nuraft_mesg
