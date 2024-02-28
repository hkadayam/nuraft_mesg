#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <boost/uuid/string_generator.hpp>
#include <sisl/grpc/generic_service.hpp>
#include <libnuraft/cluster_config.hxx>
#include <libnuraft/raft_server.hxx>

#include <nuraft_mesg/client_factory.hpp>
#include <nuraft_mesg/dcs_state_mgr.hpp>
#include "common_lib.hpp"

namespace nuraft_mesg {

DataChannel::DataChannel(shared< ClientFactory > factory) : factory_{std::move(factory)} {}

void DataChannel::on_group_changed(peer_id_t const& peer_id) {
    std::unique_lock lg{peer_mtx_};
    peer_clients_.insert_or_assign(peer_id, factory_->create_get_client(peer_id));
}

// The endpoint field of the raft_server config is the uuid of the server.
std::string DataChannel::id_to_str(int32_t id) const {
    auto const& srv_config = state_mgr_->raft_server()->get_config()->get_server(id);
    return (srv_config) ? srv_config->get_endpoint() : std::string();
}

std::optional< Result< peer_id_t > > DataChannel::get_peer_id(destination_t const& dest) const {
    if (std::holds_alternative< peer_id_t >(dest)) {
        return std::get< peer_id_t >(dest);
    } else if (std::holds_alternative< role_regex >(dest)) {
        if (state_mgr_ == nullptr) {
            LOGW("Attempting to send to role regex on non-existing replica group");
            return folly::makeUnexpected(nuraft::cmd_result_code::SERVER_NOT_FOUND);
        }

        switch (std::get< role_regex >(dest)) {
        case role_regex::LEADER: {
            if (state_mgr_->raft_server()->is_leader()) {
                LOGW("Attempting to send to leader from leader itself - perhaps leader switched now?");
                return folly::makeUnexpected(nuraft::cmd_result_code::BAD_REQUEST);
            }
            auto const leader = state_mgr_->raft_server()->get_leader();
            if (leader == -1) {
                LOGW("Leader not found - perhaps leader switched now?");
                return folly::makeUnexpected(nuraft::cmd_result_code::SERVER_NOT_FOUND);
            }
            return boost::uuids::string_generator()(id_to_str(leader));
        } break;

        case role_regex::ALL_FOLLOWERS: {
            if (!state_mgr_->raft_server()->is_leader()) {
                LOGW("Sender is not a leader anymore, perhaps leader switched now?, failing the request");
                return folly::makeUnexpected(nuraft::cmd_result_code::NOT_LEADER);
            }
            return std::nullopt;
        } break;

        case role_regex::ANY_FOLLOWER: {
            if (!state_mgr_->raft_server()->is_leader()) {
                LOGW("Sender is not a leader anymore, perhaps leader switched now?, failing the request");
                return folly::makeUnexpected(nuraft::cmd_result_code::NOT_LEADER);
            }

            std::shared_lock lg(peer_mtx_);
            if (peer_clients_.size() == 0) {
                LOGW("Attempting to send to data - but no follower client registered");
                return folly::makeUnexpected(nuraft::cmd_result_code::SERVER_NOT_FOUND);
            }
            // Get the beginning, given unordered_map begin can be random, returned peer ids could be random
            return peer_clients_.begin()->first;
        } break;

        default: {
            LOGE("Method not implemented");
            return folly::makeUnexpected(nuraft::cmd_result_code::BAD_REQUEST);
        } break;
        }
    }
    DEBUG_ASSERT(false, "Unknown destination type");
    return folly::makeUnexpected(nuraft::cmd_result_code::BAD_REQUEST);
}

} // namespace nuraft_mesg
