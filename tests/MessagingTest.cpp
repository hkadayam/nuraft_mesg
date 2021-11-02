#include <memory>
#include <string>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <grpc_helper/rpc_client.hpp>
#include <sds_logging/logging.h>
#include <sds_options/options.h>
#include <nlohmann/json.hpp>

#include <utility/thread_buffer.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "libnuraft/cluster_config.hxx"
#include "libnuraft/state_machine.hxx"
#include "messaging.h"

#include "test_state_manager.h"

SDS_LOGGING_INIT(nuraft, sds_msg, grpc_server)

SDS_OPTIONS_ENABLE(logging)

constexpr auto rpc_backoff = 50;
constexpr auto heartbeat_period = 100;
constexpr auto elect_to_low = heartbeat_period * 2;
constexpr auto elect_to_high = elect_to_low * 2;

namespace sds::messaging {

extern nuraft::ptr< nuraft::cluster_config > fromClusterConfig(nlohmann::json const& cluster_config);

static nuraft::ptr< nuraft::buffer > create_message(nlohmann::json const& j_obj) {
    auto j_str = j_obj.dump();
    auto v_msgpack = nlohmann::json::to_msgpack(j_obj);
    auto buf = nuraft::buffer::alloc(v_msgpack.size() + sizeof(int32_t));
    buf->put(&v_msgpack[0], v_msgpack.size());
    buf->pos(0);
    return buf;
}

} // namespace sds::messaging

using namespace sds::messaging;
using testing::_;
using testing::Return;

class MessagingFixture : public ::testing::Test {
protected:
    std::unique_ptr< service > instance_1;
    std::unique_ptr< service > instance_2;
    std::unique_ptr< service > instance_3;

    std::shared_ptr< test_state_mgr > sm_int_1;
    std::shared_ptr< test_state_mgr > sm_int_2;
    std::shared_ptr< test_state_mgr > sm_int_3;

    std::string id_1;
    std::string id_2;
    std::string id_3;

    std::function< std::string(std::string const&) > lookup_callback;
    nuraft::raft_params r_params;

    void SetUp() override {
        id_1 = to_string(boost::uuids::random_generator()());
        id_2 = to_string(boost::uuids::random_generator()());
        id_3 = to_string(boost::uuids::random_generator()());

        instance_1 = std::make_unique< service >();
        instance_2 = std::make_unique< service >();
        instance_3 = std::make_unique< service >();

        lookup_callback = [srv_1 = id_1, srv_2 = id_2, srv_3 = id_3](std::string const& id) -> std::string {
            if (id == srv_1)
                return "127.0.0.1:9001";
            else if (id == srv_2)
                return "127.0.0.1:9002";
            else if (id == srv_3)
                return "127.0.0.1:9003";
            else
                return std::string();
        };

        auto params = consensus_component::params{id_1, 9001, lookup_callback, "test_type"};
        instance_1->start(params);

        // RAFT server parameters
        r_params.with_election_timeout_lower(elect_to_low)
            .with_election_timeout_upper(elect_to_high)
            .with_hb_interval(heartbeat_period)
            .with_max_append_size(10)
            .with_rpc_failure_backoff(rpc_backoff)
            .with_auto_forwarding(true)
            .with_snapshot_enabled(0);

        auto register_params = consensus_component::register_params{
            r_params,
            [this, srv_addr = id_1](int32_t const srv_id,
                                    std::string const& group_id) -> std::shared_ptr< mesg_state_mgr > {
                sm_int_1 = std::make_shared< test_state_mgr >(srv_id, srv_addr, group_id);
                return std::static_pointer_cast< mesg_state_mgr >(sm_int_1);
            }};
        instance_1->register_mgr_type("test_type", register_params);

        params.server_uuid = id_2;
        params.mesg_port = 9002;
        register_params.create_state_mgr =
            [this, srv_addr = id_2](int32_t const srv_id,
                                    std::string const& group_id) -> std::shared_ptr< mesg_state_mgr > {
            sm_int_2 = std::make_shared< test_state_mgr >(srv_id, srv_addr, group_id);
            return std::static_pointer_cast< mesg_state_mgr >(sm_int_2);
        };
        instance_2->start(params);
        instance_2->register_mgr_type("test_type", register_params);

        params.server_uuid = id_3;
        params.mesg_port = 9003;
        register_params.create_state_mgr =
            [this, srv_addr = id_3](int32_t const srv_id,
                                    std::string const& group_id) -> std::shared_ptr< mesg_state_mgr > {
            sm_int_3 = std::make_shared< test_state_mgr >(srv_id, srv_addr, group_id);
            return std::static_pointer_cast< mesg_state_mgr >(sm_int_3);
        };
        instance_3->start(params);
        instance_3->register_mgr_type("test_type", register_params);

        instance_1->create_group("test_group", "test_type");

        EXPECT_TRUE(instance_1->add_member("test_group", id_2));
        EXPECT_TRUE(instance_1->add_member("test_group", id_3));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    void TearDown() override {
        instance_1.reset();
        instance_2.reset();
        instance_3.reset();
    }

public:
};

// Basic client request (append_entries)
TEST_F(MessagingFixture, ClientRequest) {
    auto buf = sds::messaging::create_message(nlohmann::json{
        {"op_type", 2},
    });
    EXPECT_FALSE(instance_1->client_request("test_group", buf));

    instance_3->leave_group("test_group");
    instance_2->leave_group("test_group");
    instance_1->leave_group("test_group");
}

// Basic resiliency test (append_entries)
TEST_F(MessagingFixture, ClientReset) {
    // Simulate a Member crash
    instance_3 = std::make_unique< service >();

    // Commit message
    auto buf = sds::messaging::create_message(nlohmann::json{
        {"op_type", 2},
    });
    EXPECT_FALSE(instance_1->client_request("test_group", buf));

    auto register_params = consensus_component::register_params{
        r_params, [](int32_t const srv_id, std::string const& group_id) -> std::shared_ptr< mesg_state_mgr > {
            throw std::logic_error("Not Supposed To Happen");
        }};
    instance_3->register_mgr_type("test_type", register_params);
    auto params = consensus_component::params{id_3, 9003, lookup_callback, "test_type"};
    instance_3->start(params);
    instance_3->join_group("test_group", "test_type", std::dynamic_pointer_cast< mesg_state_mgr >(sm_int_3));

    auto const sm1_idx = sm_int_1->get_state_machine()->last_commit_index();
    auto sm3_idx = sm_int_3->get_state_machine()->last_commit_index();
    LOGINFO("SM1: {} / SM3: {}", sm1_idx, sm3_idx);
    while (sm1_idx > sm3_idx) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        sm3_idx = sm_int_3->get_state_machine()->last_commit_index();
        LOGINFO("SM1: {} / SM3: {}", sm1_idx, sm3_idx);
    }
    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto err = instance_3->client_request("test_group", buf);
    if (err) { LOGERROR("Failed to commit: {}", err.message()); }
    EXPECT_FALSE(err);

    instance_3->leave_group("test_group");
    instance_2->leave_group("test_group");
    instance_1->leave_group("test_group");
}

// Test sending a message for a group the messaging service is not aware of.
TEST_F(MessagingFixture, UnknownGroup) {
    EXPECT_FALSE(instance_1->add_member("unknown_group", to_string(boost::uuids::random_generator()())));

    instance_1->leave_group("unknown_group");

    auto buf = sds::messaging::create_message(nlohmann::json{
        {"op_type", 2},
    });
    EXPECT_TRUE(instance_1->client_request("unknown_group", buf));
}

TEST_F(MessagingFixture, RemoveMember) {
    EXPECT_TRUE(instance_1->rem_member("test_group", id_3));

    auto buf = sds::messaging::create_message(nlohmann::json{
        {"op_type", 2},
    });
    EXPECT_FALSE(instance_1->client_request("test_group", buf));
}

int main(int argc, char* argv[]) {
    SDS_OPTIONS_LOAD(argc, argv, logging)
    ::testing::InitGoogleTest(&argc, argv);
    sds_logging::SetLogger(std::string(argv[0]));
    spdlog::set_pattern("[%D %T.%f%z] [%^%l%$] [%t] %v");
    sds_logging::GetLogger()->flush_on(spdlog::level::level_enum::err);

    auto ret = RUN_ALL_TESTS();
    grpc_helper::GrpcAsyncClientWorker::shutdown_all();

    return ret;
}
