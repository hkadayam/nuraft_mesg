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
#include "lib/client.hpp"
#include "lib/service.hpp"
#include "lib/generated/nuraft_mesg_config_generated.h"

#include "messaging_service.grpc.pb.h"
#include "utils.hpp"

SETTINGS_INIT(nuraftmesgcfg::NuraftMesgConfig, nuraft_mesg_config);

#define NURAFT_MESG_CONFIG_WITH(...) SETTINGS(nuraft_mesg_config, __VA_ARGS__)
#define NURAFT_MESG_CONFIG_THIS(...) SETTINGS_THIS(nuraft_mesg_config, __VA_ARGS__)
#define NURAFT_MESG_CONFIG(...) SETTINGS_VALUE(nuraft_mesg_config, __VA_ARGS__)

#define NURAFT_MESG_SETTINGS_FACTORY() SETTINGS_FACTORY(nuraft_mesg_config)

namespace nuraft_mesg {

std::string group_factory::m_ssl_cert;
using handle_resp = std::function< void(RaftMessage&, ::grpc::Status&) >;

std::atomic_uint64_t DCSClient::_client_counter = 0ul;

class DCSClientProtob : public DSCClientGrpc, public std::enable_shared_from_this< DCSClientProtob > {
public:
    DCSClientProtob(std::string const& worker_name, std::string const& addr,
                    const std::shared_ptr< sisl::GrpcTokenClient > token_client, std::string const& target_domain = "",
                    std::string const& ssl_cert = "") :
            DCSClientGrpc(worker_name, addr, token_client, target_domain, ssl_cert) {}
    ~DCSClientProtob() override = default;

    using DSCClientGrpc::send;

    std::atomic_uint bad_service;

    void send(RaftGroupMsg const& message, handle_resp complete) {
        auto weak_this = std::weak_ptr< DCSClientProtob >(shared_from_this());
        auto group_compl = [weak_this, complete](auto response, auto status) mutable {
            if (::grpc::INVALID_ARGUMENT == status.error_code()) {
                if (auto dc = weak_this.lock(); dc) {
                    dc->bad_service.fetch_add(1, std::memory_order_relaxed);
                    LOGE("Sent message to wrong service, need to disconnect! Error Message: [{}] Client IP: [{}]",
                         status.error_message(), dc->_addr);
                } else {
                    LOGE("Sent message to wrong service, need to disconnect! Error Message: [{}]",
                         status.error_message());
                }
            }
            complete(*response.mutable_msg(), status);
        };

        stub_->call_unary< RaftGroupMsg, RaftGroupMsg >(
            message, &Messaging::StubInterface::AsyncRaftStep, group_compl,
            NURAFT_MESG_CONFIG(mesg_factory_config->raft_request_deadline_secs));
    }
};

inline LogEntry* fromLogEntry(nuraft::log_entry const& entry, LogEntry* log) {
    log->set_term(entry.get_term());
    log->set_type((LogType)entry.get_val_type());
    auto& buffer = entry.get_buf();
    buffer.pos(0);
    log->set_buffer(buffer.data(), buffer.size());
    log->set_timestamp(entry.get_timestamp());
    return log;
}

inline RCRequest* fromRCRequest(nuraft::req_msg& rcmsg) {
    auto req = new RCRequest;
    req->set_last_log_term(rcmsg.get_last_log_term());
    req->set_last_log_index(rcmsg.get_last_log_idx());
    req->set_commit_index(rcmsg.get_commit_idx());
    for (auto& rc_entry : rcmsg.log_entries()) {
        auto entry = req->add_log_entries();
        fromLogEntry(*rc_entry, entry);
    }
    return req;
}

inline std::shared_ptr< nuraft::resp_msg > toResponse(RaftMessage const& raft_msg) {
    if (!raft_msg.has_rc_response()) return nullptr;
    auto const& base = raft_msg.base();
    auto const& resp = raft_msg.rc_response();
    auto message = std::make_shared< grpc_resp >(base.term(), (nuraft::msg_type)base.type(), base.src(), base.dest(),
                                                 resp.next_index(), resp.accepted());
    message->set_result_code((nuraft::cmd_result_code)(0 - resp.result_code()));
    if (nuraft::cmd_result_code::NOT_LEADER == message->get_result_code()) {
        LOGI("Leader has changed!");
        message->dest_addr = resp.dest_addr();
    }
    if (0 < resp.context().length()) {
        auto ctx_buffer = nuraft::buffer::alloc(resp.context().length());
        memcpy(ctx_buffer->data(), resp.context().data(), resp.context().length());
        message->set_ctx(ctx_buffer);
    }
    return message;
}

class GroupClientProtob : public GroupClient {
public:
    GroupClientProtob(std::shared_ptr< DSCClient > dsc_client, peer_id_t const& client_addr, group_id_t const& grp_name,
                      group_type_t const& grp_type) :
            GroupClient(std::move(dsc_client), client_addr, grp_name, grp_type) {}

    ~GroupClientProtob() override = default;

    ///
    // This is where the magic of serialization happens starting with creating a RaftMessage and invoking our
    // specific ::send() which will later transform into a RaftGroupMsg
    void send(std::shared_ptr< nuraft::req_msg >& req, nuraft::rpc_handler& complete, uint64_t) override {
        assert(req && complete);
        RaftMessage grpc_request;
        grpc_request.set_allocated_base(fromBaseRequest(*req));
        grpc_request.set_allocated_rc_request(fromRCRequest(*req));

        LOGT("Sending [{}] from: [{}] to: [{}]",
             nuraft::msg_type_to_string(nuraft::msg_type(grpc_request.base().type())), grpc_request.base().src(),
             grpc_request.base().dest());

        send(grpc_request, [req, complete](RaftMessage& response, ::grpc::Status& status) mutable -> void {
            std::shared_ptr< nuraft::rpc_exception > err;
            std::shared_ptr< nuraft::resp_msg > resp;

            if (status.ok()) {
                resp = toResponse(response);
                if (!resp) { err = std::make_shared< nuraft::rpc_exception >("missing response", req); }
            } else {
                err = std::make_shared< nuraft::rpc_exception >(status.error_message(), req);
            }
            complete(resp, err);
        });
    }

private:
    void send(RaftMessage const& message, handle_resp complete) {
        RaftGroupMsg group_msg;

        LOGT("Sending [{}] from: [{}] to: [{}] Group: [{}]",
             nuraft::msg_type_to_string(nuraft::msg_type(message.base().type())), message.base().src(),
             message.base().dest(), _group_id);
        if (_metrics) { COUNTER_INCREMENT(*_metrics, group_sends, 1); }
        group_msg.set_intended_addr(_client_addr);
        group_msg.set_group_id(to_string(_group_id));
        group_msg.set_group_type(_group_type);
        group_msg.mutable_msg()->CopyFrom(message);

        std::static_pointer_cast< DSCClientProtob >(nexus_client_)->send(group_msg, complete);
    }
};
} // namespace nuraft_mesg
