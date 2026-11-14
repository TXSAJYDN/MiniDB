#include <gtest/gtest.h>

#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "../src/protocol/HttpServer.h"
#include "../src/protocol/KvServer.h"
#include "../src/storage/KvStore.h"
#include "../src/storage/RdbManager.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unistd.h>

using namespace minidb;
namespace fs = std::filesystem;

class IntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir = "/tmp/minidb_integration_test_" +
                   std::to_string(static_cast<long long>(::getpid()));
        std::error_code ec;
        fs::remove_all(test_dir, ec);
        fs::create_directories(test_dir);
        rdb_path = test_dir + "/test.rdb";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir, ec);
    }

    std::string test_dir;
    std::string rdb_path;
};

TEST_F(IntegrationTest, SaveAndLoadCyclePreservesBinaryValues) {
    std::string binary_value("bin");
    binary_value.push_back(static_cast<char>(0));
    binary_value.push_back(static_cast<char>(0xff));
    binary_value.append("ary");

    {
        EventLoop loop;
        HttpServer server(&loop, InetAddress(0), rdb_path);
        auto store = std::make_shared<KvStore>();
        store->set("key1", "value1");
        store->set("binary", binary_value);
        store->set("key3", "value3");
        server.set_kv_store(store);

        EXPECT_EQ(RdbManager(rdb_path).save_checked(*server.kv_store()),
                  RdbSaveResult::Ok);
        EXPECT_TRUE(fs::exists(rdb_path));
    }

    {
        EventLoop loop;
        HttpServer server(&loop, InetAddress(0), rdb_path);
        ASSERT_TRUE(server.load_rdb());
        ASSERT_NE(server.kv_store(), nullptr);
        EXPECT_EQ(server.kv_store()->size(), 3u);
        EXPECT_EQ(server.kv_store()->get("key1"),
                  std::optional<std::string>("value1"));
        EXPECT_EQ(server.kv_store()->get("binary"),
                  std::optional<std::string>(binary_value));
        EXPECT_EQ(server.kv_store()->get("key3"),
                  std::optional<std::string>("value3"));
    }
}

TEST_F(IntegrationTest, EmptyDatabase) {
    EventLoop loop;
    HttpServer server(&loop, InetAddress(0), rdb_path);

    EXPECT_EQ(RdbManager(rdb_path).save_checked(*server.kv_store()),
              RdbSaveResult::Ok);
    EXPECT_TRUE(fs::exists(rdb_path));

    ASSERT_TRUE(server.load_rdb());
    EXPECT_EQ(server.kv_store()->size(), 0u);
}

TEST_F(IntegrationTest, LoadNonExistentStartsEmpty) {
    EventLoop loop;
    HttpServer server(&loop, InetAddress(0), rdb_path);

    EXPECT_TRUE(server.load_rdb());
    EXPECT_FALSE(fs::exists(rdb_path));
    EXPECT_EQ(server.kv_store()->size(), 0u);
}

TEST_F(IntegrationTest, MultipleSavesReplaceSnapshot) {
    EventLoop loop;
    HttpServer server(&loop, InetAddress(0), rdb_path);

    auto store = std::make_shared<KvStore>();
    server.set_kv_store(store);

    store->set("a", "1");
    EXPECT_EQ(RdbManager(rdb_path).save_checked(*server.kv_store()),
              RdbSaveResult::Ok);

    store->set("b", "2");
    EXPECT_EQ(RdbManager(rdb_path).save_checked(*server.kv_store()),
              RdbSaveResult::Ok);

    store->set("c", "3");
    EXPECT_EQ(RdbManager(rdb_path).save_checked(*server.kv_store()),
              RdbSaveResult::Ok);

    ASSERT_TRUE(fs::exists(rdb_path));
    EventLoop load_loop;
    HttpServer loaded(&load_loop, InetAddress(0), rdb_path);
    ASSERT_TRUE(loaded.load_rdb());
    EXPECT_EQ(loaded.kv_store()->size(), 3u);
    EXPECT_EQ(loaded.kv_store()->get("a"), std::optional<std::string>("1"));
    EXPECT_EQ(loaded.kv_store()->get("b"), std::optional<std::string>("2"));
    EXPECT_EQ(loaded.kv_store()->get("c"), std::optional<std::string>("3"));
}

TEST_F(IntegrationTest, SaveAndLoadWithTTLPreservesDeadline) {
    {
        EventLoop loop;
        HttpServer server(&loop, InetAddress(0), rdb_path);

        auto store = std::make_shared<KvStore>();
        store->set("persistent", "forever");
        store->set("expiring", "temporary");
        ASSERT_TRUE(store->expire("expiring", 3600000));
        server.set_kv_store(store);

        EXPECT_EQ(RdbManager(rdb_path).save_checked(*server.kv_store()),
                  RdbSaveResult::Ok);
    }

    {
        EventLoop loop;
        HttpServer server(&loop, InetAddress(0), rdb_path);
        ASSERT_TRUE(server.load_rdb());
        EXPECT_EQ(server.kv_store()->get("persistent"),
                  std::optional<std::string>("forever"));
        EXPECT_EQ(server.kv_store()->get("expiring"),
                  std::optional<std::string>("temporary"));

        const auto ttl = server.kv_store()->ttl_info("expiring");
        EXPECT_EQ(ttl.state, KvStore::TtlState::kExpiring);
        EXPECT_GT(ttl.remaining_ms, 0);
        EXPECT_LE(ttl.remaining_ms, 3600000);
        EXPECT_EQ(server.kv_store()->ttl_info("persistent").state,
                  KvStore::TtlState::kNoExpiry);
    }
}

TEST_F(IntegrationTest, ServerScannerLeasesAreReferenceCounted) {
    EventLoop loop;
    auto store = std::make_shared<KvStore>();
    auto second_store = std::make_shared<KvStore>();
    HttpServer first(&loop, InetAddress(0), rdb_path);
    HttpServer second(&loop, InetAddress(0), rdb_path);
    KvServer kv(&loop, InetAddress(0), rdb_path);
    first.set_kv_store(store);
    second.set_kv_store(store);
    kv.set_kv_store(second_store);

    EXPECT_FALSE(store->scanner_running());
    EXPECT_FALSE(second_store->scanner_running());

    first.start();
    first.start();
    EXPECT_TRUE(store->scanner_running());
    second.start();
    kv.start();
    EXPECT_TRUE(store->scanner_running());
    EXPECT_TRUE(second_store->scanner_running());

    first.stop();
    first.stop();
    EXPECT_TRUE(store->scanner_running());
    second.stop();
    EXPECT_FALSE(store->scanner_running());
    kv.stop();
    EXPECT_FALSE(second_store->scanner_running());

    // A stopped server can acquire a fresh lease.
    first.start();
    EXPECT_TRUE(store->scanner_running());
    first.stop();
    EXPECT_FALSE(store->scanner_running());
}

TEST_F(IntegrationTest, ServerDestructorsReleaseScannerLeases) {
    auto http_store = std::make_shared<KvStore>();
    auto kv_store = std::make_shared<KvStore>();
    {
        EventLoop loop;
        {
            HttpServer server(&loop, InetAddress(0), rdb_path);
            server.set_kv_store(http_store);
            server.start();
            EXPECT_TRUE(http_store->scanner_running());
        }
        EXPECT_FALSE(http_store->scanner_running());

        {
            KvServer server(&loop, InetAddress(0), rdb_path);
            server.set_kv_store(kv_store);
            server.start();
            EXPECT_TRUE(kv_store->scanner_running());
        }
        EXPECT_FALSE(kv_store->scanner_running());
    }
}

TEST_F(IntegrationTest, ScannerLeaseCanReplaceStoreOnlyBeforeStart) {
    EventLoop loop;
    HttpServer server(&loop, InetAddress(0), rdb_path);
    server.start();
    EXPECT_THROW(server.set_kv_store(std::make_shared<KvStore>()),
                 std::logic_error);
    server.stop();
}
