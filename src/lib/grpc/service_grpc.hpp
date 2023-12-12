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

#include "lib/service.hpp"

namespace nuraft_mesg {
class DCSRaftServiceGrpc : public DCSRaftService {
private:
    shared< sisl::GrpcServer > grpc_server_;

public:
    DCSRaftServiceGrpc(std::shared_ptr< DCSManagerImpl > const& manager, group_id_t const& service_address,
                       std::string const& default_group_type);
    void start(DCSManager::Params const& params) override;
    void shutdown override();

    shared< sisl::GrpcServer > server() { return grpc_server_; }
};

class DCSDataServiceGrpc : public DCSDataService {
private:
    shared< sisl::GrpcServer > grpc_server_;
    std::unordered_map< std::string, sisl::generic_rpc_handler_cb_t > request_map_;
    data_lock_type req_lock_;

public:
    DCSDataServiceGrpc();
    void start();
    void shutdown();
    bool bind_request(std::string const& request_name, group_id_t const& group_id,
                      data_channel_request_handler_t const& request_handler) override;

private:
    void rebind_requests();
};
} // namespace nuraft_mesg