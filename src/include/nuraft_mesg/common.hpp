#pragma once
#include <memory>

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/intrusive_ptr.hpp>

#include <folly/Expected.h>
#include <folly/small_vector.h>
#include <folly/Unit.h>
#include <folly/futures/Future.h>

#include <libnuraft/async.hxx>
#include <sisl/fds/buffer.hpp>
#include <sisl/logging/logging.h>

SISL_LOGGING_DECL(nuraft)
SISL_LOGGING_DECL(nuraft_mesg)

#define NURAFTMESG_LOG_MODS nuraft, nuraft_mesg, grpc_server

namespace nuraft_mesg {

using peer_id_t = boost::uuids::uuid;
using group_id_t = boost::uuids::uuid;
using channel_id_t = boost::uuids::uuid;
using group_type_t = std::string;

using io_blob_list_t = folly::small_vector< sisl::io_blob, 4 >;

template < typename T >
using Result = folly::Expected< T, nuraft::cmd_result_code >;
template < typename T >
using AsyncResult = folly::SemiFuture< Result< T > >;

using NullResult = Result< folly::Unit >;
using NullAsyncResult = AsyncResult< folly::Unit >;

ENUM(role_regex, uint8_t, LEADER, ALL_FOLLOWERS, ANY_FOLLOWER);
using destination_t = std::variant< peer_id_t, role_regex >;

template < typename T >
using shared = std::shared_ptr< T >;

template < typename T >
using cshared = const std::shared_ptr< T >;

template < typename T >
using unique = std::unique_ptr< T >;

template < typename T >
using weak = std::weak_ptr< T >;

template < typename T >
using intrusive = boost::intrusive_ptr< T >;

template < typename T >
using cintrusive = const boost::intrusive_ptr< T >;

using rpc_id_t = int64_t;
static constexpr rpc_id_t invalid_rpc_id = (rpc_id_t)-1;

} // namespace nuraft_mesg

namespace fmt {
template <>
struct formatter< nuraft_mesg::group_id_t > {
    template < typename ParseContext >
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template < typename FormatContext >
    auto format(nuraft_mesg::group_id_t const& n, FormatContext& ctx) {
        return format_to(ctx.out(), "{}", to_string(n));
    }
};

template <>
struct formatter< nuraft::cmd_result_code > {
    template < typename ParseContext >
    constexpr auto parse(ParseContext& ctx) {
        return ctx.begin();
    }

    template < typename FormatContext >
    auto format(nuraft::cmd_result_code const& c, FormatContext& ctx) {
        return format_to(ctx.out(), "{}", int32_t(c));
    }
};
} // namespace fmt
