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
    DCSRaftServiceGrpc(shared< DCSManagerImpl > manager, DCSManager::Params const& params);
    void start() override;
    void shutdown() override;

    shared< sisl::GrpcServer > server() { return grpc_server_; }
};

class DCSDataServiceGrpc : public DCSDataService {
private:
    shared< sisl::GrpcServer > grpc_server_;
    std::unordered_map< channel_id_t, shared< DataChannel >, uuid_hasher > channel_map_;
    folly::SharedMutex map_lock_;

public:
    DCSDataServiceGrpc(shared< DCSManagerImpl > manager, DCSManager::Params const& params,
                       shared< DCSRaftServiceGrpc > raft_service);
    void start() override;
    void shutdown() override;
    shared< DataChannel > create_data_channel(channel_id_t const& channel_id);
    void remove_data_channel(channel_id_t const& channel_id) override;
    rpc_id_t register_rpc(std::string const& rpc_name, group_id_t const& group_id,
                          data_channel_rpc_handler_t const& handler) override;

private:
    void reregister_rpcs();
};
} // namespace nuraft_mesg