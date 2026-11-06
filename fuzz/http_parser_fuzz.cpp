#include "../src/base/Buffer.h"
#include "../src/protocol/HttpParser.h"
#include "../src/protocol/HttpRequest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    // The application rejects requests at 1 MiB bodies.  A slightly larger
    // harness ceiling keeps pathological fuzzer allocations bounded while
    // still exercising every configured protocol limit.
    if (size > (2u * 1024u * 1024u)) {
        return 0;
    }

    minidb::Buffer buffer(size + 64u);
    minidb::HttpParser parser;
    minidb::HttpRequest request;

    std::size_t offset = 0;
    int completed_requests = 0;
    while (offset < size && !parser.has_error() && completed_requests < 16) {
        const std::size_t chunk =
            std::min<std::size_t>(1u + (data[offset] % 257u), size - offset);
        buffer.append(reinterpret_cast<const char*>(data + offset), chunk);
        offset += chunk;

        while (!parser.has_error() && parser.parse_request(&buffer, &request)) {
            if (!parser.got_all()) {
                break;
            }
            ++completed_requests;
            parser.reset();
            if (completed_requests >= 16 || buffer.readable_bytes() == 0) {
                break;
            }
        }
    }
    return 0;
}
