#include <puzzpool/log_redaction.hpp>

#include <cctype>
#include <cstddef>
#include <string>

namespace puzzpool {

namespace {

constexpr const char* kReplacement = "?<redacted>";

bool isSpace(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

} // namespace

std::string redactQueryStrings(const std::string& message) {
    std::string out;
    out.reserve(message.size());

    std::size_t pos = 0;
    while (pos < message.size()) {
        const std::size_t mark = message.find('?', pos);
        if (mark == std::string::npos) {
            out.append(message, pos, std::string::npos);
            break;
        }

        out.append(message, pos, mark - pos);
        out.append(kReplacement);

        // Skip the query string itself. A URL in a log line is delimited by
        // whitespace, so the first space after the '?' ends it.
        std::size_t end = mark + 1;
        while (end < message.size() && !isSpace(message[end])) ++end;
        pos = end;
    }

    return out;
}

} // namespace puzzpool
