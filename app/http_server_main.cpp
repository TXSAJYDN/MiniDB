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
        << "  -r <rdb_path>  RDB file path (default: minidb.rdb)\n"
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
    loop.quit_on_signals({SIGINT, SIGTERM});

    InetAddress listen_addr(options.port);
    const std::string rdb_path =
        options.rdb_path.empty() ? "minidb.rdb" : options.rdb_path;
    HttpServer server(&loop, listen_addr, rdb_path);

    std::cout << "Loading RDB from: " << rdb_path << '\n';
    if (!server.load_rdb()) {
        std::cerr << "Fatal: Failed to load RDB file (file corrupted or "
                     "inaccessible). Aborting startup."
                  << '\n';
        return 1;
    }
    std::cout << "RDB loaded successfully" << '\n';

    std::cout << "\nMiniDB HTTP server starting on port " << options.port
              << '\n';
    std::cout << "\nEndpoints:" << '\n';
    std::cout << "  GET    /health      - Health check" << '\n';
    std::cout << "  GET    /kv/<key>    - Get value" << '\n';
    std::cout << "  POST   /kv/<key>    - Set value (body as value)" << '\n';
    std::cout << "  DELETE /kv/<key>    - Delete key" << '\n';
    std::cout << "  POST   /save        - Save database to RDB" << '\n';
    std::cout << "\nPress Ctrl+C to stop\n" << '\n';

    server.start();
    loop.loop();

    std::cout << "\nServer stopped; persisting RDB" << '\n';
    const RdbSaveResult save_result = server.save_rdb();
    if (save_result != RdbSaveResult::Ok) {
        std::cerr << "Fatal: failed to persist RDB during shutdown"
                  << (save_result == RdbSaveResult::DurabilityUnconfirmed
                          ? " (snapshot replaced, durability unconfirmed)"
                          : "")
                  << '\n';
        return 1;
    }
    std::cout << "Server stopped; RDB saved" << '\n';
    return 0;
}
