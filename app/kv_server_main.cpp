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
    out << "Usage: " << program << " [-p port] [-r rdb_path]\n"
        << "Defaults: -p 6379, -r minidb.rdb\n";
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
    loop.quit_on_signals({SIGINT, SIGTERM});
    InetAddress listen_addr(options.port);
    const std::string rdb_path =
        options.rdb_path.empty() ? "minidb.rdb" : options.rdb_path;
    KvServer server(&loop, listen_addr, rdb_path);

    if (!server.load_rdb()) {
        std::cerr << "Fatal: failed to load RDB file; refusing to start\n";
        return 1;
    }

    std::cout << "MiniDB KV server listening on port " << options.port << "\n";
    std::cout << "RDB path: " << rdb_path << "\n";
    server.start();
    loop.loop();

    // Join the scanner before the final snapshot.  save_rdb() itself is
    // linearizable, but explicit shutdown ordering keeps the exit path
    // deterministic and avoids any background work during persistence.
    server.stop();
    const RdbSaveResult save_result = server.save_rdb();
    if (save_result != RdbSaveResult::Ok) {
        std::cerr << "Fatal: failed to persist RDB during shutdown"
                  << (save_result == RdbSaveResult::DurabilityUnconfirmed
                          ? " (snapshot replaced, durability unconfirmed)"
                          : "")
                  << "\n";
        return 1;
    }
    std::cout << "Server stopped; RDB saved\n";
    return 0;
}
