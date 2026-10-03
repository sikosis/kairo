#include "kairo/diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

namespace kairo {
namespace {

std::string Lowercase(const std::string& text) {
//---------------------------------------------------------------------------------------------------------------------------------//

    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return lower;
}

bool ContainsSensitiveLabel(const std::string& lower) {
//---------------------------------------------------------------------------------------------------------------------------------//

    static const char* labels[] = {
        "authorization:", "bearer ", "access_token", "refresh_token",
        "id_token", "api_key", "api key", "client_secret", "code_verifier"
    };
    for (const char* label : labels)
        if (lower.find(label) != std::string::npos) return true;
    return false;
}

void RedactUrlQueries(std::string& text) {
//---------------------------------------------------------------------------------------------------------------------------------//

    std::size_t search = 0;
    while (search < text.size()) {
        const std::size_t plain = text.find("http://", search);
        const std::size_t secure = text.find("https://", search);
        std::size_t start = plain;
        if (start == std::string::npos || (secure != std::string::npos && secure < start))
            start = secure;
        if (start == std::string::npos) break;
        const std::size_t end = text.find_first_of(" \t\r\n\"'<>]})", start);
        const std::size_t query = text.find('?', start);
        if (query != std::string::npos && (end == std::string::npos || query < end)) {
            const std::size_t query_end = end == std::string::npos ? text.size() : end;
            text.replace(query, query_end - query, "?[redacted]");
            search = query + 11;
        } else {
            search = end == std::string::npos ? text.size() : end;
        }
    }
}

void RedactJwtLikeValues(std::string& text) {
//---------------------------------------------------------------------------------------------------------------------------------//

    std::size_t start = 0;
    while (start < text.size()) {
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
        std::size_t end = start;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        if (end - start >= 64 &&
            std::count(text.begin() + static_cast<std::ptrdiff_t>(start),
                       text.begin() + static_cast<std::ptrdiff_t>(end), '.') >= 2) {
            text.replace(start, end - start, "[redacted-token]");
            end = start + 16;
        }
        start = end;
    }
}

}  // namespace

std::string SanitiseDiagnostic(std::string text) {
//---------------------------------------------------------------------------------------------------------------------------------//

    for (char& character : text) {
        const unsigned char value = static_cast<unsigned char>(character);
        if (character == '\n' || character == '\r' || character == '\t') character = ' ';
        else if (value < 0x20 || value == 0x7f) character = '?';
    }
    if (ContainsSensitiveLabel(Lowercase(text))) return "[redacted sensitive diagnostic]";
    RedactUrlQueries(text);
    RedactJwtLikeValues(text);
    constexpr std::size_t kMaximumDiagnosticLength = 2048;
    if (text.size() > kMaximumDiagnosticLength)
        text.resize(kMaximumDiagnosticLength);
    return text;
}

}  // namespace kairo
