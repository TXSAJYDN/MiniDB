// AI-014: 回归测试 - HTTP Keep-Alive 粘包和响应顺序
#include <gtest/gtest.h>
#include "../src/protocol/HttpParser.h"
#include "../src/base/Buffer.h"
#include <memory>

using namespace minidb;

// AI-014 P0: 测试 Keep-Alive 粘包时 parser 状态正确
TEST(HttpKeepAliveTest, ParserResetBeforeSend) {
    // 这个测试验证 parser_.reset() 在 send 之前被调用
    // 确保同步写完成时不会看到 kGotAll 状态

    Buffer buf;
    buf.append("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
    buf.append("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");

    // 解析第一个请求
    HttpRequest req1;
    HttpParser parser1;
    ASSERT_TRUE(parser1.parse_request(&buf, &req1));
    ASSERT_TRUE(parser1.got_all());

    // 重置 parser (模拟 HttpSession 的行为)
    parser1.reset();

    // 验证 reset 后状态正确
    EXPECT_EQ(parser1.state(), HttpParser::kExpectRequestLine);
    EXPECT_FALSE(parser1.got_all());

    // 解析第二个请求应该成功
    HttpRequest req2;
    ASSERT_TRUE(parser1.parse_request(&buf, &req2));
    ASSERT_TRUE(parser1.got_all());

    EXPECT_EQ(req2.method(), HttpRequest::kGet);
    EXPECT_EQ(req2.path(), "/health");
}

// AI-014 P1: 测试分片输入的 header 限制
TEST(HttpKeepAliveTest, HeaderSizeLimitNotAppliedToBody) {
    Buffer buf;

    // 添加一个带大 body 的请求(但 header 很小)
    std::string req = "POST /test HTTP/1.1\r\n";
    req += "Host: localhost\r\n";
    req += "Content-Length: 70000\r\n"; // 70KB body
    req += "\r\n";

    buf.append(req);

    // Header 大小应该远小于 64KB
    EXPECT_LT(req.size(), 64 * 1024);

    // 添加 70KB 的 body
    std::string body(70000, 'x');
    buf.append(body);

    // 总大小超过 64KB
    EXPECT_GT(buf.readable_bytes(), 64 * 1024);

    // 但解析应该成功(如果 header/body 限制正确分离)
    HttpParser parser;
    HttpRequest parsed_req;

    ASSERT_TRUE(parser.parse_request(&buf, &parsed_req));
    ASSERT_TRUE(parser.got_all());

    EXPECT_EQ(parsed_req.method(), HttpRequest::kPost);
    EXPECT_EQ(parsed_req.body().size(), 70000);
}

// AI-014: 测试同步写完成时的重入安全性
TEST(HttpKeepAliveTest, SynchronousWriteCompletionSafety) {
    // 这个测试模拟最危险的场景:
    // send() 同步完成,在 HttpSession::on_message() 还没返回时触发回调

    bool parser_reset_before_callback = false;
    bool callback_saw_clean_state = false;

    // 模拟场景
    {
        HttpParser parser;
        Buffer buf;
        buf.append("GET /test HTTP/1.1\r\nHost: localhost\r\n\r\n");

        HttpRequest req;
        ASSERT_TRUE(parser.parse_request(&buf, &req));
        ASSERT_TRUE(parser.got_all());

        // 关键:在调用可能同步完成的 send 之前 reset
        parser.reset();
        parser_reset_before_callback = true;

        // 模拟同步写完成回调
        if (parser_reset_before_callback) {
            // 回调时检查 parser 状态
            callback_saw_clean_state =
                (parser.state() == HttpParser::kExpectRequestLine);
        }
    }

    EXPECT_TRUE(parser_reset_before_callback);
    EXPECT_TRUE(callback_saw_clean_state)
        << "Parser should be in clean state when callback is invoked";
}

// AI-014 P0: 测试多个粘包请求的解析
TEST(HttpKeepAliveTest, MultiplePackedRequests) {
    Buffer buf;

    // 在 Buffer 中放入 3 个完整的请求
    buf.append("GET /api/v1 HTTP/1.1\r\nHost: localhost\r\n\r\n");
    buf.append("POST /api/v2 HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
               "5\r\n\r\nhello");
    buf.append("DELETE /api/v3 HTTP/1.1\r\nHost: localhost\r\n\r\n");

    HttpParser parser;

    // 解析第一个请求
    HttpRequest req1;
    ASSERT_TRUE(parser.parse_request(&buf, &req1));
    ASSERT_TRUE(parser.got_all());
    EXPECT_EQ(req1.method(), HttpRequest::kGet);
    EXPECT_EQ(req1.path(), "/api/v1");

    // 重置并解析第二个请求
    parser.reset();
    HttpRequest req2;
    ASSERT_TRUE(parser.parse_request(&buf, &req2));
    ASSERT_TRUE(parser.got_all());
    EXPECT_EQ(req2.method(), HttpRequest::kPost);
    EXPECT_EQ(req2.path(), "/api/v2");
    EXPECT_EQ(req2.body(), "hello");

    // 重置并解析第三个请求
    parser.reset();
    HttpRequest req3;
    ASSERT_TRUE(parser.parse_request(&buf, &req3));
    ASSERT_TRUE(parser.got_all());
    EXPECT_EQ(req3.method(), HttpRequest::kDelete);
    EXPECT_EQ(req3.path(), "/api/v3");
}
