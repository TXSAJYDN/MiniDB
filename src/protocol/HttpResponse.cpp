#include "HttpResponse.h"

#include "../base/Buffer.h"
#include <string>

namespace minidb {

void HttpResponse::append_to_buffer(Buffer* output) const {
    if (output == nullptr) {
        return;
    }

    output->append("HTTP/1.1 ");
    output->append(std::to_string(static_cast<int>(status_code_)));
    output->append(" ");
    output->append(status_message_);
    output->append("\r\n");

    if (close_connection_) {
        output->append("Connection: close\r\n");
    } else {
        output->append("Connection: Keep-Alive\r\n");
    }

    // Content-Length is mandatory on every response, including close
    // responses, so clients can frame the final error/body deterministically.
    // HEAD deliberately keeps this length while suppressing the body bytes.
    output->append("Content-Length: ");
    output->append(std::to_string(body_.size()));
    output->append("\r\n");

    for (const auto& header : headers_) {
        output->append(header.first);
        output->append(": ");
        output->append(header.second);
        output->append("\r\n");
    }

    output->append("\r\n");
    if (!suppress_body_) {
        output->append(body_);
    }
}

} // namespace minidb
