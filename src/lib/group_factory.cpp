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
#include <libnuraft/async.hxx>
#include <string>

#include <folly/futures/Future.h>
#include <sisl/settings/settings.hpp>

#include "nuraft_mesg/client_factory.hpp"

namespace nuraft_mesg {
nuraft::ptr< nuraft::rpc_client > GroupClientFactory::create_client(std::string const& client_id) {
    nuraft::ptr< nuraft::rpc_client > client;
    auto code = create_client(boost::uuids::string_generator()(client_id), client);
    if (code != nuraft::OK) { LOGC("Client Endpoint Invalid! [{}], error={}", client_id, code); }
    return client;
}

nuraft::cmd_result_code GroupClientFactory::create_client(peer_id_t const& client_id,
                                                          nuraft::ptr< nuraft::rpc_client >& raft_client) {
    // Re-direct this call to a global factory so we can re-use clients to the same endpoints
    LOGD("Creating/Getting client to peer {}", client_id);
    auto nexus_client = common_factory_->create_get_client(client_id);
    if (!nexus_client) return nuraft::CANCELLED;

    // Create a raft group specific client by attaching the nexus_client
    raft_client = std::make_shared< GroupClient >(nexus_client, client_id, group_id_, group_type_);
    if (!raft_client) { return nuraft::BAD_REQUEST; }

    if (group_change_cb_) { group_change_cb_(client_id, nexus_client); }
    return nuraft::OK;
}

nuraft::cmd_result_code GroupClientFactory::reinit_client(peer_id_t const& client_id,
                                                          std::shared_ptr< nuraft::rpc_client >& raft_client) {
    LOGD("Re-init client to peer {}", client_id);
    auto grp_client = std::dynamic_pointer_cast< GroupClient >(raft_client);
    auto new_client = std::static_pointer_cast< nuraft::rpc_client >(grp_client->realClient());

    if (auto err = common_factory_->reinit_client(client, new_client); err) { return err; }
    grp_client->setClient(new_client);

    if (group_change_cb_) { group_change_cb_(client_id, nexus_client); }
    return nuraft::OK;
}
} // namespace nuraft_mesg