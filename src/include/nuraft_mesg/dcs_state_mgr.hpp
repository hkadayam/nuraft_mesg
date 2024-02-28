#pragma once

#include <unordered_map>
#include <memory>

#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <libnuraft/state_mgr.hxx>
#include <grpcpp/grpcpp.h>
#include <sisl/grpc/rpc_common.hpp>
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

class DCSStateManager;
class ClientFactory;
class DCSClient;

class DataChannel {
protected:
    shared< ClientFactory > factory_;
    mutable folly::SharedMutex peer_mtx_;
    std::map< peer_id_t, shared< DCSClient > > peer_clients_;
    DCSStateManager* state_mgr_{nullptr};

public:
    DataChannel(shared< ClientFactory > factory);
    virtual ~DataChannel() = default;

    void on_group_changed(peer_id_t const& peer_id);
    void set_state_mgr(DCSStateManager* mgr) { state_mgr_ = mgr; }

    virtual std::pair< rpc_id_t, bool > add_rpc(std::string const& rpc_name,
                                                sisl::generic_rpc_handler_cb_t handler) = 0;
    virtual void close_rpc_registration() = 0;
    virtual NullAsyncResult unidirectional_request(destination_t const& dest, rpc_id_t rpc_id,
                                                   io_blob_list_t const& cli_buf) = 0;
    virtual AsyncResult< sisl::io_blob > bidirectional_request(destination_t const& dest, rpc_id_t rpc_id,
                                                               io_blob_list_t const& cli_buf) = 0;

    // Send response to a data channel request and finish the async call.
    virtual void send_response(io_blob_list_t const& outgoing_buf, intrusive< sisl::GenericRpcData >& rpc_data) = 0;

    std::optional< Result< peer_id_t > > get_peer_id(destination_t const& dest) const;

private:
    std::string id_to_str(int32_t id) const;
};

class RaftGroupServer;

class DCSStateManager : public nuraft::state_mgr {
private:
    shared< DataChannel > data_channel_;
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
    virtual void register_data_channel_rpcs(shared< DataChannel > channel) = 0;

    void set_raft_group_server(shared< RaftGroupServer > group_server);
    bool is_raft_leader() const;

    bool has_data_channel() const { return (data_channel_ == nullptr); }
    void set_data_channel(shared< DataChannel > channel) {
        data_channel_ = std::move(channel);
        register_data_channel_rpcs(data_channel_);
        data_channel_->close_rpc_registration();
    }

    void reset_data_channel() { data_channel_.reset(); }

    nuraft::raft_server* raft_server();
    DataChannel& data_channel() {
        DEBUG_ASSERT(has_data_channel(), "Attempting to get data channel without initializing");
        return *data_channel_;
    }
};

} // namespace nuraft_mesg
