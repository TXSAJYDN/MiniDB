#include <gtest/gtest.h>
#include "../src/protocol/HttpParser.h"
#include "../src/protocol/HttpRequest.h"
#include "../src/base/Buffer.h"

#include <string>

namespace minidb {

class HttpParserP0Test : public ::testing::Test {
  protected:
    struct Result {
        bool ok;
        bool got_all;
        HttpParser::ParseError error;
        size_t remaining;
    };

    Result Parse(const std::string& raw, HttpRequest* req = nullptr) {
        parser_.reset();
        Buffer buffer;
        buffer.append(raw);
        HttpRequest local;
        HttpRequest* target = req == nullptr ? &local : req;
        const bool ok = parser_.parse_request(&buffer, target);
        return {ok, parser_.got_all(), parser_.parse_error(),
                buffer.readable_bytes()};
    }

    HttpParser parser_;
};

TEST_F(HttpParserP0Test, AcceptValidPostWithHostAndBinarySafeBody) {
    const std::string body("a\0b\xff", 4);
    HttpRequest request;
    const Result result = Parse("POST /kv/key HTTP/1.1\r\nHost: "
                                "localhost\r\nContent-Length: 4\r\n\r\n" +
                                    body,
                                &request);
    EXPECT_TRUE(result.ok);
    EXPECT_TRUE(result.got_all);
    EXPECT_EQ(result.error, HttpParser::ParseError::kNone);
    EXPECT_EQ(result.remaining, 0u);
    EXPECT_EQ(request.method(), HttpRequest::kPost);
    EXPECT_EQ(request.path(), "/kv/key");
    EXPECT_EQ(request.body(), body);
}

TEST_F(HttpParserP0Test, RequireExactlyOneValidHost) {
    EXPECT_EQ(Parse("GET / HTTP/1.1\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET / HTTP/1.1\r\nHost: bad host\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET / HTTP/1.1\r\nHost: localhost:70000\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_TRUE(Parse("GET / HTTP/1.1\r\nHost: [::1]:8080\r\n\r\n").got_all);
}

TEST_F(HttpParserP0Test, DuplicateContentLengthRules) {
    HttpRequest request;
    const Result equal =
        Parse("POST / HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 5\r\nContent-Length: 05\r\n\r\nhello",
              &request);
    EXPECT_TRUE(equal.ok);
    EXPECT_TRUE(equal.got_all);
    EXPECT_EQ(request.get_header("Content-Length"), "5");
    EXPECT_EQ(request.body(), "hello");

    const Result different =
        Parse("POST / HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello!");
    EXPECT_FALSE(different.ok);
    EXPECT_EQ(different.error, HttpParser::ParseError::kBadRequest);
}

TEST_F(HttpParserP0Test, RejectInvalidOrConflictingFraming) {
    EXPECT_EQ(Parse("POST / HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Length: abc\r\n\r\n")
                  .error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("POST / HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Length: -1\r\n\r\n")
                  .error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("POST / HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n")
                  .error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET / HTTP/1.1\r\nHost: localhost\r\n"
                    "Content-Length: 1\r\n\r\nx")
                  .error,
              HttpParser::ParseError::kBadRequest);
}

TEST_F(HttpParserP0Test, UnsupportedFramingFeaturesAreLeftToSession) {
    const Result transfer =
        Parse("POST / HTTP/1.1\r\nHost: localhost\r\nTransfer-Encoding: "
              "chunked\r\n\r\n");
    EXPECT_TRUE(transfer.ok);
    EXPECT_TRUE(transfer.got_all);
    EXPECT_EQ(transfer.error, HttpParser::ParseError::kNone);

    const Result expect = Parse(
        "POST / HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\n\r\n");
    EXPECT_TRUE(expect.ok);
    EXPECT_TRUE(expect.got_all);

    const Result upgrade = Parse(
        "GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n\r\n");
    EXPECT_TRUE(upgrade.ok);
    EXPECT_TRUE(upgrade.got_all);
}

TEST_F(HttpParserP0Test, VersionAndRequestLineErrorsMapToAccurateStatuses) {
    EXPECT_EQ(Parse("GET / HTTP/2.0\r\nHost: localhost\r\n\r\n").error,
              HttpParser::ParseError::kVersionNotSupported);
    EXPECT_EQ(Parse("GET / HTTP/1.10\r\nHost: localhost\r\n\r\n").error,
              HttpParser::ParseError::kVersionNotSupported);
    EXPECT_EQ(Parse("GET / HTTP/1\r\nHost: localhost\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET / HTTP/1.1x\r\nHost: localhost\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(
        Parse("GET http://example/ HTTP/1.1\r\nHost: localhost\r\n\r\n").error,
        HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET  / HTTP/1.1\r\nHost: localhost\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(Parse("GET / HTTP/1.1 extra\r\nHost: localhost\r\n\r\n").error,
              HttpParser::ParseError::kBadRequest);
}

TEST_F(HttpParserP0Test, RejectMalformedHeaderSyntax) {
    EXPECT_EQ(
        Parse("GET / HTTP/1.1\r\nHost: localhost\r\nBad Header: x\r\n\r\n")
            .error,
        HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(
        Parse("GET / HTTP/1.1\r\nHost: localhost\r\nNoColon\r\n\r\n").error,
        HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(
        Parse("GET / HTTP/1.1\r\nHost: localhost\r\n X-Fold: x\r\n\r\n").error,
        HttpParser::ParseError::kBadRequest);
    EXPECT_EQ(
        Parse("GET / HTTP/1.1\r\nHost: localhost\r\nX: a\rb\r\n\r\n").error,
        HttpParser::ParseError::kBadRequest);
}

TEST_F(HttpParserP0Test, RequestTargetLimitMapsTo414) {
    std::string target =
        "/" + std::string(HttpParser::kMaxRequestTargetSize, 'a');
    const std::string raw =
        "GET " + target + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    EXPECT_EQ(Parse(raw).error, HttpParser::ParseError::kUriTooLong);
}

TEST_F(HttpParserP0Test, UnterminatedRequestLineFragmentOverLimitMapsTo414) {
    Buffer buffer;
    HttpRequest request;
    const std::string fragment(HttpParser::kMaxRequestLineSize, 'R');
    buffer.append(fragment);
    ASSERT_FALSE(buffer.find_crlf());
    EXPECT_TRUE(parser_.parse_request(&buffer, &request));
    EXPECT_FALSE(parser_.has_error());

    buffer.append("X", 1);
    ASSERT_FALSE(buffer.find_crlf());
    EXPECT_FALSE(parser_.parse_request(&buffer, &request));
    EXPECT_EQ(parser_.parse_error(), HttpParser::ParseError::kUriTooLong);
}

TEST_F(HttpParserP0Test, UnterminatedHeaderFragmentOverLimitMapsTo431) {
    Buffer buffer;
    HttpRequest request;
    buffer.append("GET / HTTP/1.1\r\n");
    ASSERT_TRUE(parser_.parse_request(&buffer, &request));
    ASSERT_FALSE(parser_.has_error());
    ASSERT_EQ(buffer.readable_bytes(), 0u);

    const std::string fragment(HttpParser::kMaxHeaderSize + 1, 'v');
    buffer.append(fragment);
    ASSERT_FALSE(buffer.find_crlf());
    EXPECT_FALSE(parser_.parse_request(&buffer, &request));
    EXPECT_EQ(parser_.parse_error(), HttpParser::ParseError::kHeaderTooLarge);
}

TEST_F(HttpParserP0Test, AccumulatedCompleteHeadersOverLimitMapsTo431) {
    Buffer buffer;
    HttpRequest request;
    buffer.append("GET / HTTP/1.1\r\n");
    ASSERT_TRUE(parser_.parse_request(&buffer, &request));
    ASSERT_FALSE(parser_.has_error());

    const std::string value(6000, 'h');
    for (int i = 0; i < 3; ++i) {
        buffer.append("X-Fill-" + std::to_string(i) + ": " + value + "\r\n");
        const bool ok = parser_.parse_request(&buffer, &request);
        if (i < 2) {
            EXPECT_TRUE(ok);
            EXPECT_FALSE(parser_.has_error());
            EXPECT_LE(parser_.header_bytes_consumed(),
                      HttpParser::kMaxHeaderSize);
        } else {
            EXPECT_FALSE(ok);
            EXPECT_EQ(parser_.parse_error(),
                      HttpParser::ParseError::kHeaderTooLarge);
        }
    }
}

TEST_F(HttpParserP0Test, HeaderFieldCountLimitIs64) {
    std::string exactly_64 = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (int i = 0; i < 63; ++i) {
        exactly_64 += "X-" + std::to_string(i) + ": v\r\n";
    }
    exactly_64 += "\r\n";
    EXPECT_TRUE(Parse(exactly_64).got_all);

    std::string sixty_five = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (int i = 0; i < 64; ++i) {
        sixty_five += "X-" + std::to_string(i) + ": v\r\n";
    }
    sixty_five += "\r\n";
    EXPECT_EQ(Parse(sixty_five).error, HttpParser::ParseError::kHeaderTooLarge);
}

TEST_F(HttpParserP0Test, BodyLimitAcceptanceAndRejection) {
    const std::string accepted_body(
        static_cast<size_t>(HttpParser::kMaxBodySize), 'x');
    HttpRequest accepted_request;
    const Result accepted = Parse(
        "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
            std::to_string(accepted_body.size()) + "\r\n\r\n" + accepted_body,
        &accepted_request);
    EXPECT_TRUE(accepted.ok);
    EXPECT_TRUE(accepted.got_all);
    EXPECT_EQ(accepted_request.body().size(), HttpParser::kMaxBodySize);

    const Result rejected =
        Parse("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
              "1048577\r\n\r\n");
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, HttpParser::ParseError::kPayloadTooLarge);
}

TEST_F(HttpParserP0Test, IncompleteBodyWaitsForMoreData) {
    HttpRequest request;
    const Result result = Parse("POST /echo HTTP/1.1\r\nHost: "
                                "localhost\r\nContent-Length: 5\r\n\r\nabc",
                                &request);
    EXPECT_TRUE(result.ok);
    EXPECT_FALSE(result.got_all);
    EXPECT_EQ(result.error, HttpParser::ParseError::kNone);
    EXPECT_EQ(result.remaining, 3u);
}

TEST_F(HttpParserP0Test, UnterminatedRequestLineBeyond64KiBIsRejectedAs414) {
    // AI-018: prove the incomplete-request-line path rejects a fragment larger
    // than 64 KiB even though the current P0 contract rejects it earlier.
    Buffer buffer;
    HttpRequest request;
    const std::string fragment(65537, 'R');
    buffer.append(fragment);
    ASSERT_FALSE(buffer.find_crlf());
    EXPECT_FALSE(parser_.parse_request(&buffer, &request));
    EXPECT_TRUE(parser_.has_error());
    EXPECT_EQ(parser_.parse_error(), HttpParser::ParseError::kUriTooLong);
}

TEST_F(HttpParserP0Test, UnterminatedHeaderBeyond64KiBIsRejectedAs431) {
    // AI-018: the request line is consumed first; the remaining 65537-byte
    // header fragment has no CRLF and must still be bounded immediately.
    Buffer buffer;
    HttpRequest request;
    buffer.append("GET / HTTP/1.1\r\n");
    ASSERT_TRUE(parser_.parse_request(&buffer, &request));
    ASSERT_FALSE(parser_.has_error());
    ASSERT_EQ(buffer.readable_bytes(), 0u);

    const std::string fragment(65537, 'v');
    buffer.append(fragment);
    ASSERT_FALSE(buffer.find_crlf());
    EXPECT_FALSE(parser_.parse_request(&buffer, &request));
    EXPECT_TRUE(parser_.has_error());
    EXPECT_EQ(parser_.parse_error(), HttpParser::ParseError::kHeaderTooLarge);
}

} // namespace minidb
