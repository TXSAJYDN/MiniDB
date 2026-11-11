#include "../src/protocol/HttpServer.h"
#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "ServerOptions.h"

#include <csignal>
#include <iostream>
#include <ostream>
#include <string>

using namespace minidb;

namespace {

void print_usage(const char* prog, std::ostream& out) {
    out << "Usage: " << prog << " [options]\n"
        << "Options:\n"
        << "  -p <port>      Port to listen on (default: 8080)\n"
        << "  -r <rdb_path>  Persistence base path (default: minidb.rdb)\n"
        << "  -d <mode>      Durability: sync or interval (default: sync)\n"
        << "  -h             Show this help\n";
}

} // namespace

int main(int argc, char* argv[]) {
    // MSG_NOSIGNAL is used by TcpConnection; this is defense in depth for any
    // future socket write path.
    std::signal(SIGPIPE, SIG_IGN);

    ServerOptions options;
    std::string parse_error;
    if (!parse_server_options(argc, argv, 8080, &options, &parse_error)) {
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
    HttpServer server(&loop, listen_addr, rdb_path, options.durability_mode);

    // Block termination signals before any persistence or scanner thread is
    // created.  Those threads inherit this mask; signalfd owns the only
    // unblocked path and wakes the owner loop for a graceful shutdown.
    loop.quit_on_signals({SIGINT, SIGTERM},
                         [&server]() { server.begin_shutdown(); });

    std::cout << "Opening persistence image: " << rdb_path << '\n';
    std::cout << "Durability mode: "
              << durability_mode_name(options.durability_mode) << '\n';
    std::cout << "Checkpoint manifest: " << rdb_path << ".manifest\n";
    std::cout << "WAL directory: " << rdb_path << ".wal/\n";
    if (!server.load_rdb()) {
        std::cerr << "Fatal: failed to open the persistence image. Check the "
                     "RDB, manifest, WAL directory, and single-writer lock; "
                     "see the recovery error above.\n";
        return 1;
    }
    std::cout << "Persistence image recovered successfully\n";

    std::cout << "\nMiniDB HTTP server starting on port " << options.port
              << '\n';
    std::cout << "\nEndpoints:\n";
    std::cout << "  GET    /health      - Health check\n";
    std::cout << "  GET    /kv/<key>    - Get value\n";
    std::cout << "  POST   /kv/<key>    - Set value (body as value)\n";
    std::cout << "  DELETE /kv/<key>    - Delete key\n";
    std::cout << "  POST   /save        - Publish a durable RDB checkpoint\n";
    std::cout << "\nPress Ctrl+C to stop\n\n";

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
                  << '\n';
        return 1;
    }
    std::cout << "Server stopped; checkpoint saved\n";
    return 0;
}
