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

#include <sisl/grpc/rpc_client.hpp>
#include "lib/client.hpp"
#include "messaging_service.grpc.pb.h"

namespace nuraft_mesg {
class DCSClientGrpc : public DCSClient, public sisl::GrpcAsyncClient {
public:
    DCSClientGrpc(std::string const& worker_name, std::string const& addr,
                  const std::shared_ptr< sisl::GrpcTokenClient > token_client, std::string const& target_domain = "",
                  std::string const& ssl_cert = "");

    void init() override;
    sisl::GrpcAsyncClient::GenericAsyncStub& generic_stub() { return *generic_stub_; }
    bool is_connection_ready() const override { return sisl::GrpcAsyncClient::is_connection_ready(); }

protected:
    typename sisl::GrpcAsyncClient::AsyncStub< DCSMessaging >::UPtr stub_;
    std::unique_ptr< sisl::GrpcAsyncClient::GenericAsyncStub > generic_stub_;
};

} // namespace nuraft_mesg