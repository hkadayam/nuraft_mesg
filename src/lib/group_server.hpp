#pragma once

#include <libnuraft/raft_server_handler.hxx>
#include <grpcpp/server.h>
#include <nuraft_mesg/common.hpp>

namespace sisl {
class GrpcServer;
}

namespace nuraft_mesg {

class GroupMetrics : public sisl::MetricsGroup {
public:
    explicit GroupMetrics(group_id_t const& group_id) : sisl::MetricsGroup("RAFTGroup", to_string(group_id).c_str()) {
        REGISTER_COUNTER(group_steps, "Total group messages received", "raft_group", {"op", "step"});
        REGISTER_COUNTER(group_sends, "Total group messages sent", "raft_group", {"op", "send"});
        register_me_to_farm();
    }

    ~GroupMetrics() { deregister_me_from_farm(); }
};

// Brief:
//   Translates and forwards the gRPC Step() to cornerstone's raft_server::step()
//
class RaftGroupServer {
    std::shared_ptr< nuraft::raft_server > raft_server_;
    GroupMetrics metrics_;

public:
    RaftGroupServer(std::shared_ptr< nuraft::raft_server >& raft_server, group_id_t const& group_id) :
            raft_server_(raft_server), metrics_{group_id} {}
    virtual ~RaftGroupServer() = default;
    RaftGroupServer(const RaftGroupServer&) = delete;
    RaftGroupServer& operator=(const RaftGroupServer&) = delete;

    nuraft::ptr< nuraft::cmd_result< nuraft::ptr< nuraft::buffer > > > add_srv(const nuraft::srv_config& cfg);

    nuraft::ptr< nuraft::cmd_result< nuraft::ptr< nuraft::buffer > > > rem_srv(int const member_id);

    bool request_leadership();
    void yield_leadership(bool immediate = false);

    void get_srv_config_all(std::vector< nuraft::ptr< nuraft::srv_config > >& configs_out);

    nuraft::ptr< nuraft::cmd_result< nuraft::ptr< nuraft::buffer > > >
    append_entries(const std::vector< nuraft::ptr< nuraft::buffer > >& logs);

    std::shared_ptr< nuraft::raft_server > raft_server() { return raft_server_; }
};

} // namespace nuraft_mesg
