#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// NIS (yp_*, <rpcsvc/ypclnt.h>) and the Sun RPC client calls Python's nis module links:
// Alchemy runs no ypbind and no portmapper, so there is no NIS domain and no RPC server to
// reach. Lookups answer as on a Mac without NIS: yp_get_default_domain YPERR_NODOM, the map
// calls YPERR_YPBIND; clnt_create fails with RPC_PMAPFAILURE and clnt_spcreateerror describes
// it. The XDR codecs are only reached through a client handle, which never exists, so they
// stop explicitly.
namespace alchemy::export_fill {
namespace {

// <rpcsvc/ypclnt.h>
constexpr std::uint64_t yperr_ypbind = 10, yperr_nodom = 12;
constexpr std::array<const char*, 17> yp_errors{
    "Success", "Request arguments bad", "RPC failure", "Can't bind to server which serves this domain",
    "No such map in server's domain", "No such key in map", "YP server error", "Local resource allocation failure",
    "No more records in map database", "Can't communicate with portmapper", "Can't communicate with ypbind",
    "Can't communicate with ypserv", "Local domain name not set", "Server data base is bad",
    "YP server version mismatch", "Access violation", "Database is busy"};
// <rpc/clnt.h> enum clnt_stat, as clnt_sperrno words them.
constexpr std::uint32_t rpc_pmapfailure = 14;
const std::map<std::uint32_t, std::string> rpc_errors{
    {0, "RPC: Success"}, {1, "RPC: Can't encode arguments"}, {2, "RPC: Can't decode result"},
    {3, "RPC: Unable to send"}, {4, "RPC: Unable to receive"}, {5, "RPC: Timed out"},
    {6, "RPC: Incompatible versions of RPC"}, {7, "RPC: Authentication error"}, {8, "RPC: Program unavailable"},
    {9, "RPC: Program/version mismatch"}, {10, "RPC: Procedure unavailable"},
    {11, "RPC: Server can't decode arguments"}, {12, "RPC: Remote system error"}, {13, "RPC: Unknown host"},
    {14, "RPC: Port mapper failure"}, {15, "RPC: Program not registered"}, {16, "RPC: Failed (unspecified error)"},
    {17, "RPC: Unknown protocol"}};

} // namespace

void install_libsystem_rpc_nis(Provider& p) {
    auto& process = p.process();

    // int yp_get_default_domain(char** domain)
    p.libsystem("_yp_get_default_domain", [](CpuState&, GuestMemory&) -> std::uint64_t { return yperr_nodom; });
    // The map calls need a bound NIS server (yp_match, yp_first, yp_next, yp_all, yp_order,
    // yp_master, yp_bind); yp_unbind has nothing to release.
    for (const auto* name : {"_yp_match", "_yp_first", "_yp_next", "_yp_all", "_yp_order", "_yp_master", "_yp_bind"})
        p.libsystem(name, [](CpuState&, GuestMemory&) -> std::uint64_t { return yperr_ypbind; });
    p.libsystem("_yp_unbind", [](CpuState&, GuestMemory&) -> std::uint64_t { return 0; });
    // char* yperr_string(int code)
    std::vector<std::uint64_t> yp_texts;
    for (const auto* text : yp_errors) {
        const auto address = process.allocate_guest(std::string_view(text).size() + 1);
        put_string(process.main_engine(), address, text);
        yp_texts.push_back(address);
    }
    const auto unknown_yp = process.allocate_guest(32);
    put_string(process.main_engine(), unknown_yp, "Unknown NIS error code");
    p.libsystem("_yperr_string", [yp_texts, unknown_yp](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto code = static_cast<std::uint32_t>(s.x[0]);
        return code < yp_texts.size() ? yp_texts[code] : unknown_yp;
    });

    // CLIENT* clnt_create(const char* host, rpcprog_t, rpcvers_t, const char* protocol)
    const auto created = std::make_shared<std::pair<std::mutex, std::uint32_t>>();
    p.libsystem("_clnt_create", [created](CpuState&, GuestMemory&) -> std::uint64_t {
        std::lock_guard lock(created->first);
        created->second = rpc_pmapfailure;
        return 0;
    }, 0);
    // char* clnt_spcreateerror(const char* s): "<s>: <why clnt_create failed>\n", in a buffer
    // the next call reuses.
    const auto buffer = process.allocate_guest(1024);
    p.libsystem("_clnt_spcreateerror", [created, buffer](CpuState& s, GuestMemory& m) -> std::uint64_t {
        std::uint32_t status;
        { std::lock_guard lock(created->first); status = created->second; }
        const auto found = rpc_errors.find(status);
        auto text = (s.x[0] ? guest_string(m, s.x[0], 512) : std::string()) + ": " +
            (found == rpc_errors.end() ? std::string("RPC: (unknown error code)") : found->second);
        if (status == rpc_pmapfailure) text += " - RPC: Program not registered";
        text += "\n";
        put_string(m, buffer, text.substr(0, 1023));
        return buffer;
    }, 0);
    // char* clnt_sperrno(enum clnt_stat)
    std::map<std::uint32_t, std::uint64_t> rpc_texts;
    for (const auto& [code, text] : rpc_errors) {
        const auto address = process.allocate_guest(text.size() + 1);
        put_string(process.main_engine(), address, text);
        rpc_texts[code] = address;
    }
    p.libsystem("_clnt_sperrno", [rpc_texts](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto found = rpc_texts.find(static_cast<std::uint32_t>(s.x[0]));
        return found == rpc_texts.end() ? rpc_texts.at(16) : found->second;
    });
    for (const auto* name : {"_xdr_string", "_xdr_enum", "_xdr_pointer", "_xdr_int", "_xdr_bool", "_xdr_bytes", "_xdr_opaque"}) {
        const auto message = "Alchemy: " + std::string(name + 1) + " needs a Sun RPC client, and Alchemy has no RPC servers (no portmapper)";
        p.libsystem(name, [message](CpuState&, GuestMemory&) -> std::uint64_t { throw std::runtime_error(message); });
    }
}

} // namespace alchemy::export_fill
