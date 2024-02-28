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
#include "lib/grpc/client_grpc.hpp"

namespace nuraft_mesg {
DCSClientGrpc::DCSClientGrpc(std::string const& worker_name, std::string const& addr,
                             const std::shared_ptr< sisl::GrpcTokenClient > token_client,
                             std::string const& target_domain, std::string const& ssl_cert) :
        DCSClient(worker_name, addr), sisl::GrpcAsyncClient(addr, token_client, target_domain, ssl_cert) {
    init();
}

void DCSClientGrpc::init() {
    // Re-create channel only if current channel is busted.
    if (stub_ && is_connection_ready()) {
        LOGD("Channel looks fine, re-using");
        return;
    }
    LOGD("Client init ({}) to {}", (!!stub_ ? "Again" : "First"), addr_);
    sisl::GrpcAsyncClient::init();
    stub_ = sisl::GrpcAsyncClient::make_stub< DCSMessaging >(worker_name_);
}
} // namespace nuraft_mesg