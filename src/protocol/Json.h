#pragma once

#include <string>
#include <string_view>

namespace minidb {

// Escapes a byte string for inclusion between JSON double quotes.  Valid
// UTF-8 bytes above U+001F are preserved verbatim; callers that need strict
// Unicode validation must enforce it before calling this helper.
std::string json_escape(std::string_view value);

} // namespace minidb
