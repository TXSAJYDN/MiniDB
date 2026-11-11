#include "../src/protocol/KvServer.h"
#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "ServerOptions.h"

#include <csignal>
#include <iostream>
#include <ostream>
#include <string>

using namespace minidb;

namespace {

void print_usage(const char* program, std::ostream& out) {
    out << "Usage: " << program
        << " [-p port] [-r rdb_path] [-d sync|interval]\n"
        << "Defaults: -p 6379, -r minidb.rdb, -d sync\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    ServerOptions options;
    std::string parse_error;
    if (!parse_server_options(argc, argv, 6379, &options, &parse_error)) {
        std::cerr << "Invalid arguments: " << parse_error << "\n";
        print_usage(argv[0], std::cerr);
        return 2;
    }
    if (options.help_requested) {
        print_usage(argv[0], std::cout);
        return 0;
    }

    EventLoop loop;
    InetAddress listen_addr(options.port);
    const std::string rdb_path =
        options.rdb_path.empty() ? "minidb.rdb" : options.rdb_path;
    KvServer server(&loop, listen_addr, rdb_path, options.durability_mode);

    // Block termination signals before any persistence or scanner thread is
    // created.  Those threads inherit this mask; signalfd owns the only
    // unblocked path and wakes the owner loop for a graceful shutdown.
    loop.quit_on_signals({SIGINT, SIGTERM},
                         [&server]() { server.begin_shutdown(); });

    if (!server.load_rdb()) {
        std::cerr << "Fatal: failed to open the persistence image; check the "
                     "RDB, manifest, WAL directory, and single-writer lock "
                     "(see the recovery error above)\n";
        return 1;
    }

    std::cout << "MiniDB KV server listening on port " << options.port << "\n";
    std::cout << "Persistence path: " << rdb_path << "\n";
    std::cout << "Durability mode: "
              << durability_mode_name(options.durability_mode) << "\n";
    server.start();

    // SIGINT/SIGTERM start a non-blocking owner-loop shutdown.  The loop keeps
    // running until the writer has drained, the final checkpoint is durable,
    // and all persistence threads have been joined.
    loop.loop();

    if (!server.shutdown_complete()) {
        std::cerr << "Fatal: event loop exited before graceful shutdown\n";
        return 1;
    }
    if (server.shutdown_result() != RdbSaveResult::Ok) {
        std::cerr << "Fatal: failed to publish checkpoint during shutdown"
                  << (!server.shutdown_error().empty()
                          ? ": " + server.shutdown_error()
                          : "")
                  << (server.shutdown_result() ==
                              RdbSaveResult::DurabilityUnconfirmed
                          ? " (snapshot replaced, durability unconfirmed)"
                          : "")
                  << "\n";
        return 1;
    }
    std::cout << "Server stopped; checkpoint saved\n";
    return 0;
}
