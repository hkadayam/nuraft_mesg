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
#include <boost/uuid/string_generator.hpp>
#include <libnuraft/async.hxx>

#include <nuraft_mesg/client_factory.hpp>

#include "lib/client.hpp"

namespace nuraft_mesg {

template < typename Payload >
struct client_ctx {
    client_ctx(Payload payload, shared< ClientFactory > factory, int32_t dest,
               peer_id_t const& new_srv_addr = peer_id_t()) :
            cur_dest_(dest), new_srv_addr_(new_srv_addr), payload_(payload), cli_factory_(std::move(factory)) {}

    Payload payload() const { return payload_; }

    shared< ClientFactory > cli_factory() const { return cli_factory_; }

    NullAsyncResult future() {
        auto [p, sf] = folly::makePromiseContract< NullResult >();
        promise_ = std::move(p);
        return sf;
    }
    void set(nuraft::cmd_result_code const code) {
        if (nuraft::OK == code)
            promise_.setValue(folly::Unit());
        else
            promise_.setValue(folly::makeUnexpected(code));
    }

private:
    int32_t cur_dest_;
    peer_id_t const new_srv_addr_;

    Payload const payload_;
    shared< ClientFactory > cli_factory_;
    folly::Promise< NullResult > promise_;
};

template < typename PayloadType >
shared< nuraft::req_msg > createMessage(PayloadType payload, peer_id_t const& srv_addr = peer_id_t());

template <>
shared< nuraft::req_msg > createMessage(uint32_t const srv_id, peer_id_t const& srv_addr) {
    auto srv_conf = nuraft::srv_config(srv_id, to_string(srv_addr));
    auto log = std::make_shared< nuraft::log_entry >(0, srv_conf.serialize(), nuraft::log_val_type::cluster_server);
    auto msg = std::make_shared< nuraft::req_msg >(0, nuraft::msg_type::add_server_request, 0, 0, 0, 0, 0);
    msg->log_entries().push_back(log);
    return msg;
}

template <>
shared< nuraft::req_msg > createMessage(shared< nuraft::buffer > buf, peer_id_t const&) {
    auto log = std::make_shared< nuraft::log_entry >(0, buf);
    auto msg = std::make_shared< nuraft::req_msg >(0, nuraft::msg_type::client_request, 0, 1, 0, 0, 0);
    msg->log_entries().push_back(log);
    return msg;
}

template <>
shared< nuraft::req_msg > createMessage(int32_t const srv_id, peer_id_t const&) {
    auto buf = nuraft::buffer::alloc(sizeof(srv_id));
    buf->put(srv_id);
    buf->pos(0);
    auto log = std::make_shared< nuraft::log_entry >(0, buf, nuraft::log_val_type::cluster_server);
    auto msg = std::make_shared< nuraft::req_msg >(0, nuraft::msg_type::remove_server_request, 0, 0, 0, 0, 0);
    msg->log_entries().push_back(log);
    return msg;
}

template < typename ContextType >
void respHandler(shared< ContextType > ctx, shared< nuraft::resp_msg >& rsp, shared< nuraft::rpc_exception >& err) {
    auto factory = ctx->cli_factory();
    if (err || !rsp) {
        LOGE("{}", (err ? err->what() : "No response."));
        ctx->set((rsp ? rsp->get_result_code() : nuraft::cmd_result_code::SERVER_NOT_FOUND));
        return;
    } else if (rsp->get_accepted()) {
        LOGD("Accepted response");
        ctx->set(rsp->get_result_code());
        return;
    } else if (ctx->cur_dest_ == rsp->get_dst()) {
        LOGW("Request ignored");
        ctx->set(rsp->get_result_code());
        return;
    } else if (0 > rsp->get_dst()) {
        LOGW("No known leader!");
        ctx->set(rsp->get_result_code());
        return;
    }

    // Not accepted: means that `get_dst()` is a new leader.
    auto gresp = std::dynamic_pointer_cast< raft_resp >(rsp);
    LOGD("Updating destination from {} to {}[{}]", ctx->cur_dest_, rsp->get_dst(), gresp->dest_addr);
    ctx->cur_dest_ = rsp->get_dst();
    auto client = factory->create_get_raft_client(gresp->dest_addr);

    // We'll try again by forwarding the message
    auto handler = static_cast< nuraft::rpc_handler >(
        [ctx](shared< nuraft::resp_msg >& rsp, shared< nuraft::rpc_exception >& err) { respHandler(ctx, rsp, err); });

    LOGD("Creating new message: {}", ctx->new_srv_addr_);
    auto msg = createMessage(ctx->payload(), ctx->new_srv_addr_);
    client->send(msg, handler);
}

class ErrorClient : public GroupClient {
    void send(shared< nuraft::req_msg >& req, nuraft::rpc_handler& complete, uint64_t) override {
        auto resp = shared< nuraft::resp_msg >();
        auto error = std::make_shared< nuraft::rpc_exception >("Bad connection", req);
        complete(resp, error);
    }
};

shared< DCSClient > ClientFactory::create_get_client(std::string const& client) {
    try {
        return create_get_client(boost::uuids::string_generator()(client));
    } catch (std::runtime_error const& e) { LOGC("Client Endpoint Invalid! [{}]", client); }
    return nullptr;
}

shared< DCSClient > ClientFactory::create_get_client(peer_id_t const& client) {
    shared< DCSClient > new_client;

    std::unique_lock< client_factory_lock_type > lk(_client_lock);
    auto [it, happened] = dcs_clients_.emplace(client, nullptr);
    if (dcs_clients_.end() != it) {
        if (!happened) {
            LOGD("Re-creating client for {}", client);
            if (auto err = reinit_client(client, it->second); err == nuraft::OK) { new_client = it->second; }
        } else {
            LOGD("Creating client for {}", client);
            if (auto err = _create_raft_client(client, it->second); nuraft::OK == err) { new_client = it->second; }
        }
        if (!it->second) { dcs_clients_.erase(it); }
    }
    return new_client;
}

nuraft::cmd_result_code ClientFactory::reinit_raft_client(peer_id_t const& client,
                                                          shared< nuraft::rpc_client >& raft_client) {
    LOGD("Re-init client to {}", client);
    assert(raft_client);
    auto mesg_client = std::dynamic_pointer_cast< DSCClient >(raft_client);
    if (!mesg_client->is_connection_ready() || 0 < mesg_client->bad_service.load(std::memory_order_relaxed)) {
        return _create_client(client, raft_client);
    }
    return nuraft::OK;
}

NullAsyncResult ClientFactory::add_server(uint32_t srv_id, peer_id_t const& srv_addr,
                                          nuraft::srv_config const& dest_cfg) {
    auto client = create_get_client(dest_cfg.get_endpoint());
    if (!client) { return folly::makeUnexpected(nuraft::CANCELLED); }

    auto ctx = std::make_shared< client_ctx< uint32_t > >(srv_id, shared_from_this(), dest_cfg.get_id(), srv_addr);
    auto handler = static_cast< nuraft::rpc_handler >(
        [ctx](shared< nuraft::resp_msg >& rsp, shared< nuraft::rpc_exception >& err) { respHandler(ctx, rsp, err); });

    auto msg = createMessage(srv_id, srv_addr);
    client->send(msg, handler);
    return ctx->future();
}

NullAsyncResult ClientFactory::rem_server(uint32_t srv_id, nuraft::srv_config const& dest_cfg) {
    auto client = create_get_raft_client(dest_cfg.get_endpoint());
    if (!client) { return folly::makeUnexpected(nuraft::CANCELLED); }

    auto ctx = std::make_shared< client_ctx< int32_t > >(srv_id, shared_from_this(), dest_cfg.get_id());
    auto handler = static_cast< nuraft::rpc_handler >(
        [ctx](shared< nuraft::resp_msg >& rsp, shared< nuraft::rpc_exception >& err) { respHandler(ctx, rsp, err); });

    auto msg = createMessage(static_cast< int32_t >(srv_id));
    client->send(msg, handler);
    return ctx->future();
}

NullAsyncResult ClientFactory::append_entry(shared< nuraft::buffer > buf, nuraft::srv_config const& dest_cfg) {
    auto client = create_get_raft_client(dest_cfg.get_endpoint());
    if (!client) { return folly::makeUnexpected(nuraft::CANCELLED); }

    auto ctx = std::make_shared< client_ctx< shared< nuraft::buffer > > >(buf, shared_from_this(), dest_cfg.get_id());
    auto handler = static_cast< nuraft::rpc_handler >(
        [ctx](shared< nuraft::resp_msg >& rsp, shared< nuraft::rpc_exception >& err) { respHandler(ctx, rsp, err); });

    auto msg = createMessage(buf);
    client->send(msg, handler);
    return ctx->future();
}
} // namespace nuraft_mesg
