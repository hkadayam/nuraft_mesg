#pragma once

#include <list>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <libnuraft/state_mgr.hxx>

#include "common.hpp"

namespace nuraft {
class raft_server;
class state_machine;
} // namespace nuraft

namespace sisl {
class GenericRpcData;
}

namespace nuraft_mesg {

// config for a replica with after the int32_t id is transformed to a peer_id_t
struct replica_config {
    std::string peer_id;
    std::string aux;
};

class DataChannel {
private:
    shared< ClientFactory > factory_;
    folly::SharedMutex peer_mtx_;
    std::unordered_map< peer_id_t, shared< DCSClient > > peer_clients_;

public:
    DataChannel(shared< ClientFactory > factory) : factory_{std::move(factory)} {}
    virtual ~DataChannel() = default;

    void on_group_changed(peer_id_t const& peer_id);
    virtual NullAsyncResult unidirectional_request(destination_t const& dest, std::string const& request_name,
                                                   io_blob_list_t const& cli_buf) = 0;
    virtual AsyncResult< sisl::io_blob > bidirectional_request(destination_t const& dest,
                                                               std::string const& request_name,
                                                               io_blob_list_t const& cli_buf) = 0;

    // Send response to a data channel request and finish the async call.
    virtual void send_response(io_blob_list_t const& outgoing_buf, intrusive< sisl::GenericRpcData >& rpc_data) = 0;
};

class RaftGroupServer;

class DCSStateManager : public nuraft::state_mgr {
private:
    unique< DataChannel > data_channel_;
    shared< RaftGroupServer > group_server_;

public:
    using nuraft::state_mgr::state_mgr;

    DCSStateManager() = default;
    virtual ~DCSStateManager() = default;

    virtual void become_ready() {}
    virtual uint32_t get_logstore_id() const = 0;
    virtual std::shared_ptr< nuraft::state_machine > get_state_machine() = 0;
    virtual void permanent_destroy() = 0;
    virtual void leave() = 0;

    void set_raft_group_server(shared< RaftGroupServer > group_server);
    bool is_raft_leader() const;

    bool has_data_channel() const { return (data_channel_ == nullptr); }
    void set_data_channel(unique< DataChannel > channel) { data_channel_ = std::move(channel); }

    nuraft::raft_server* raft_server();
    DataChannel& data_channel() {
        DEBUG_ASSERT(has_data_channel(), "Attempting to get data channel without initializing");
        return *data_channel_;
    }
};

} // namespace nuraft_mesg
