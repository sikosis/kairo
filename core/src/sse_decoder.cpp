#include "kairo/sse_decoder.h"

#include <stdexcept>

namespace kairo {
namespace {

constexpr std::size_t kMaximumSseBuffer = 1024 * 1024;

}

void SseDecoder::Feed(const char* data, std::size_t size) {
    if (size > kMaximumSseBuffer || pending_.size() > kMaximumSseBuffer - size)
        throw std::runtime_error("SSE line exceeds the 1 MiB safety limit");
    pending_.append(data, size);
    std::size_t newline;
    while ((newline = pending_.find('\n')) != std::string::npos) {
        std::string line = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        ConsumeLine(line);
    }
}

void SseDecoder::Finish() {
    if (!pending_.empty()) ConsumeLine(pending_);
    pending_.clear();
    if (!event_data_.empty()) { handler_(event_data_); event_data_.clear(); }
}

void SseDecoder::ConsumeLine(const std::string& line) {
    if (line.empty()) {
        if (!event_data_.empty()) { handler_(event_data_); event_data_.clear(); }
        return;
    }
    if (line[0] == ':') return;
    if (line.rfind("data:", 0) == 0) {
        std::string data = line.substr(5);
        if (!data.empty() && data[0] == ' ') data.erase(0, 1);
        const std::size_t separator = event_data_.empty() ? 0 : 1;
        if (data.size() > kMaximumSseBuffer - separator ||
            event_data_.size() > kMaximumSseBuffer - separator - data.size())
            throw std::runtime_error("SSE event exceeds the 1 MiB safety limit");
        if (!event_data_.empty()) event_data_ += '\n';
        event_data_ += data;
    }
}

}  // namespace kairo
