#include <gtest/gtest.h>

#include "../app/ServerOptions.h"

#include <string>
#include <vector>

namespace minidb {
namespace {

struct ParsedArgs {
    std::vector<std::string> storage;
    std::vector<char*> argv;

    explicit ParsedArgs(std::vector<std::string> values)
        : storage(std::move(values)) {
        argv.reserve(storage.size());
        for (auto& value : storage) {
            argv.push_back(value.data());
        }
        argv.push_back(nullptr);
    }

    int argc() const {
        return static_cast<int>(argv.size() - 1);
    }
};

bool parse(ParsedArgs& args, uint16_t default_port, ServerOptions* options,
           std::string* error) {
    return parse_server_options(args.argc(), args.argv.data(), default_port,
                                options, error);
}

TEST(ServerOptionsTest, DefaultsArePreserved) {
    ParsedArgs args({"minidb_http_server"});
    ServerOptions options;
    std::string error;
    ASSERT_TRUE(parse(args, 8080, &options, &error));
    EXPECT_EQ(options.port, 8080u);
    EXPECT_TRUE(options.rdb_path.empty());
    EXPECT_EQ(options.durability_mode, DurabilityMode::kSync);
    EXPECT_FALSE(options.help_requested);
    EXPECT_TRUE(error.empty());
}

TEST(ServerOptionsTest, ParsesDurabilityModes) {
    for (const auto& [text, expected] :
         {std::pair<std::string, DurabilityMode>{"sync", DurabilityMode::kSync},
          {"interval", DurabilityMode::kInterval}}) {
        ParsedArgs args({"server", "-d", text});
        ServerOptions options;
        std::string error;
        ASSERT_TRUE(parse(args, 8080, &options, &error)) << text;
        EXPECT_EQ(options.durability_mode, expected);
    }
}

TEST(ServerOptionsTest, ParsesPortAndRdbInEitherOrder) {
    ParsedArgs first({"server", "-p", "18080", "-r", "/tmp/a.rdb"});
    ServerOptions first_options;
    std::string first_error;
    ASSERT_TRUE(parse(first, 8080, &first_options, &first_error));
    EXPECT_EQ(first_options.port, 18080u);
    EXPECT_EQ(first_options.rdb_path, "/tmp/a.rdb");
    EXPECT_FALSE(first_options.help_requested);

    ParsedArgs second({"server", "-r", "/tmp/b.rdb", "-p", "6379"});
    ServerOptions second_options;
    std::string second_error;
    ASSERT_TRUE(parse(second, 8080, &second_options, &second_error));
    EXPECT_EQ(second_options.port, 6379u);
    EXPECT_EQ(second_options.rdb_path, "/tmp/b.rdb");
}

TEST(ServerOptionsTest, HelpMustBeStandalone) {
    ParsedArgs help({"server", "-h"});
    ServerOptions options;
    std::string error;
    ASSERT_TRUE(parse(help, 8080, &options, &error));
    EXPECT_TRUE(options.help_requested);
    EXPECT_EQ(options.port, 8080u);

    ParsedArgs with_mode({"server", "-h", "-d", "sync"});
    ServerOptions mode_options;
    std::string mode_error;
    EXPECT_FALSE(parse(with_mode, 8080, &mode_options, &mode_error));

    ParsedArgs with_port({"server", "-h", "-p", "8080"});
    ServerOptions bad_options;
    std::string bad_error;
    EXPECT_FALSE(parse(with_port, 8080, &bad_options, &bad_error));
}

TEST(ServerOptionsTest, RejectsMissingEmptyAndDuplicateOptions) {
    for (const auto& test_args : {
             std::vector<std::string>{"server", "-p"},
             std::vector<std::string>{"server", "-r"},
             std::vector<std::string>{"server", "-r", ""},
             std::vector<std::string>{"server", "-d"},
             std::vector<std::string>{"server", "-d", ""},
             std::vector<std::string>{"server", "-p", "1", "-p", "2"},
             std::vector<std::string>{"server", "-r", "a", "-r", "b"},
             std::vector<std::string>{"server", "-d", "sync", "-d", "interval"},
         }) {
        ParsedArgs args(test_args);
        ServerOptions options;
        std::string error;
        EXPECT_FALSE(parse(args, 8080, &options, &error)) << test_args.size();
        EXPECT_FALSE(error.empty());
    }
}

TEST(ServerOptionsTest, RejectsInvalidDurabilityModes) {
    for (const char* value : {"", "SYNC", "batch", "sync ", "0"}) {
        ParsedArgs args({"server", "-d", value});
        ServerOptions options;
        std::string error;
        EXPECT_FALSE(parse(args, 8080, &options, &error)) << value;
        EXPECT_FALSE(error.empty());
    }
}

TEST(ServerOptionsTest, RejectsInvalidPorts) {
    for (const char* value :
         {"0", "65536", "+1", " 1", "1 ", "1x", "-1", "", "0x10"}) {
        ParsedArgs args({"server", "-p", value});
        ServerOptions options;
        std::string error;
        EXPECT_FALSE(parse(args, 8080, &options, &error)) << value;
        EXPECT_FALSE(error.empty());
    }
    ParsedArgs boundary({"server", "-p", "65535"});
    ServerOptions options;
    std::string error;
    ASSERT_TRUE(parse(boundary, 8080, &options, &error));
    EXPECT_EQ(options.port, 65535u);
}

TEST(ServerOptionsTest, RejectsUnknownAndPositionalArguments) {
    for (const auto& test_args : {
             std::vector<std::string>{"server", "--help"},
             std::vector<std::string>{"server", "extra"},
             std::vector<std::string>{"server", "-x", "1"},
         }) {
        ParsedArgs args(test_args);
        ServerOptions options;
        std::string error;
        EXPECT_FALSE(parse(args, 8080, &options, &error));
        EXPECT_FALSE(error.empty());
    }
}

} // namespace
} // namespace minidb
