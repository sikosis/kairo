#pragma once

#include <functional>
#include <string>

namespace kairo {

class SseDecoder {
public:
    using DataHandler = std::function<void(const std::string&)>;
    explicit SseDecoder(DataHandler handler) : handler_(std::move(handler)) {}
    void Feed(const char* data, std::size_t size);
    void Finish();

private:
    void ConsumeLine(const std::string& line);
    DataHandler handler_;
    std::string pending_;
    std::string event_data_;
};

}  // namespace kairo
