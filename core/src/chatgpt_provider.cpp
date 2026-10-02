#include "kairo/chatgpt_provider.h"

#include "json.h"
#include "kairo/chatgpt_auth.h"
#include "kairo/sse_decoder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

#if KAIRO_HAS_CURL
#include <curl/curl.h>
#endif

namespace fs = std::filesystem;

namespace kairo {
namespace {

#if KAIRO_HAS_CURL
constexpr std::size_t kMaximumResponseTextBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumToolArgumentsBytes = 1024 * 1024;
constexpr std::size_t kMaximumToolCalls = 128;

json::Value StringProperty(const std::string& description) {
    return json::Value::Object({{"type", json::Value::String("string")},
                                {"description", json::Value::String(description)}});
}

json::Value Tool(const std::string& name, const std::string& description,
                 std::map<std::string, json::Value> properties,
                 std::vector<std::string> required) {
    std::vector<json::Value> required_json;
    for (const auto& item : required) required_json.push_back(json::Value::String(item));
    return json::Value::Object({
        {"type", json::Value::String("function")}, {"name", json::Value::String(name)},
        {"description", json::Value::String(description)}, {"strict", json::Value::Boolean(false)},
        {"parameters", json::Value::Object({
            {"type", json::Value::String("object")},
            {"properties", json::Value::Object(std::move(properties))},
            {"required", json::Value::Array(std::move(required_json))},
            {"additionalProperties", json::Value::Boolean(false)},
        })},
    });
}

json::Value RequestJson(const ProviderRequest& request) {
    std::vector<json::Value> input;
    std::string instructions;
    for (const auto& message : request.messages) {
        if (message.role == Role::System) {
            if (!instructions.empty()) instructions += "\n\n";
            instructions += message.content;
            continue;
        }
        if (message.role == Role::Tool) {
            input.push_back(json::Value::Object({
                {"type", json::Value::String("function_call_output")},
                {"call_id", json::Value::String(message.tool_call_id)},
                {"output", json::Value::String(message.content)},
            }));
            continue;
        }
        for (const auto& item : message.provider_items) {
            const auto parsed = json::Parse(item);
            if (parsed.type != json::Value::Type::Object || parsed.GetString("type") != "reasoning")
                throw std::runtime_error("invalid saved ChatGPT context item");
            input.push_back(parsed);
        }
        if (!message.content.empty()) {
            input.push_back(json::Value::Object({
                {"type", json::Value::String("message")},
                {"role", json::Value::String(message.role == Role::Assistant ? "assistant" : "user")},
                {"content", json::Value::String(message.content)},
            }));
        }
        for (const auto& call : message.tool_calls) {
            input.push_back(json::Value::Object({
                {"type", json::Value::String("function_call")},
                {"call_id", json::Value::String(call.id)},
                {"name", json::Value::String(call.name)},
                {"arguments", json::Value::String(call.arguments)},
            }));
        }
    }
    std::vector<json::Value> workspace_tools;
    workspace_tools.push_back(Tool("read_file", "Read a UTF-8 file inside the selected project.",
        {{"path", StringProperty("Literal project-relative file path")}}, {"path"}));
    workspace_tools.push_back(Tool("search_files", "Search text in project files.",
        {{"query", StringProperty("Literal search text")},
         {"path", StringProperty("Optional project-relative directory; use '.' for the project root")}}, {"query"}));
    workspace_tools.push_back(Tool("write_file", "Write a project file after user approval.",
        {{"path", StringProperty("Project-relative path")},
         {"content", StringProperty("Complete replacement content")}}, {"path", "content"}));
    workspace_tools.push_back(Tool("run_shell", "Run a shell command after user approval.",
        {{"command", StringProperty("Exact shell command")},
         {"working_directory", StringProperty("Project-relative directory; use '.' for the project root")}}, {"command"}));
    std::vector<json::Value> tools;
    tools.push_back(json::Value::Object({
        {"type", json::Value::String("namespace")},
        {"name", json::Value::String("kairo_workspace")},
        {"description", json::Value::String("Read, search, edit, and run approved commands in the selected Kairo project.")},
        {"tools", json::Value::Array(std::move(workspace_tools))},
    }));
    std::map<std::string, json::Value> body{
        {"model", json::Value::String(request.model)}, {"input", json::Value::Array(std::move(input))},
        {"tools", json::Value::Array(std::move(tools))}, {"store", json::Value::Boolean(false)},
        {"stream", json::Value::Boolean(true)},
        {"include", json::Value::Array({json::Value::String("reasoning.encrypted_content")})},
    };
    if (!instructions.empty()) body["instructions"] = json::Value::String(std::move(instructions));
    return json::Value::Object(std::move(body));
}

std::string CertificateFile() {
    if (const char* value = std::getenv("CURL_CA_BUNDLE"); value && *value) return value;
    if (const char* value = std::getenv("SSL_CERT_FILE"); value && *value) return value;
#ifdef __HAIKU__
    constexpr const char* candidates[] = {
        "/boot/system/data/ssl/CARootCertificates.pem", "/system/data/ssl/CARootCertificates.pem",
        "/boot/home/config/non-packaged/data/ssl/CARootCertificates.pem",
    };
    for (const char* candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error) return candidate;
    }
#endif
    return {};
}

std::string ErrorSummary(const json::Value& root) {
    const json::Value* error = root.Find("error");
    std::string message;
    std::string code;
    std::string parameter;
    if (error && error->type == json::Value::Type::Object) {
        message = error->GetString("message");
        code = error->GetString("code");
        parameter = error->GetString("param");
    } else {
        message = root.GetString("detail", root.GetString("message"));
        code = root.GetString("code");
    }
    std::string summary = message;
    if (!code.empty()) summary += (summary.empty() ? "code " : " [code ") + code +
                                  (message.empty() ? "" : "]");
    if (!parameter.empty()) summary += " [parameter " + parameter + "]";
    if (summary.size() > 1000) summary.resize(1000);
    return summary;
}

std::string ErrorSummary(const std::string& body) {
    try { return ErrorSummary(json::Parse(body)); }
    catch (...) { return {}; }
}
#endif

}  // namespace

ChatGPTProvider::ChatGPTProvider(ChatGPTConfig config) : config_(std::move(config)) {
    if (config_.credential_file.empty()) throw std::invalid_argument("ChatGPT credential path is required");
    if (config_.timeout_seconds <= 0) throw std::invalid_argument("provider timeout must be positive");
}

ProviderResponse ChatGPTProvider::Complete(const ProviderRequest& request, const EventSink& events,
                                           const CancellationToken& cancellation) {
#if !KAIRO_HAS_CURL
    (void)request; (void)events; (void)cancellation;
    throw std::runtime_error("Kairo was built without ChatGPT network support");
#else
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("libcurl global initialization failed");
    });
    std::string token = ChatGPTAuth(config_.credential_file).AccessToken();
    std::string body = json::Encode(RequestJson(request));
    ProviderResponse response;
    std::map<std::size_t, ToolCall> calls;
    bool completed = false;
    std::string error_body;
    std::exception_ptr event_error;
    SseDecoder decoder([&](const std::string& data) {
        if (event_error || data == "[DONE]") return;
        try {
            const auto event = json::Parse(data);
            const std::string type = event.GetString("type");
            if (type == "response.output_text.delta") {
                std::string delta = event.GetString("delta");
                if (delta.size() > kMaximumResponseTextBytes - std::min(response.text.size(), kMaximumResponseTextBytes))
                    throw std::runtime_error("ChatGPT response exceeds the 4 MiB safety limit");
                response.text += delta;
                if (events && !delta.empty()) events({EventType::TextDelta, {}, {}, delta});
            } else if (type == "response.output_item.done") {
                const auto* item = event.Find("item");
                if (!item) return;
                if (item->GetString("type") == "reasoning") {
                    response.provider_items.push_back(json::Encode(*item));
                    return;
                }
                if (item->GetString("type") != "function_call") return;
                const auto* index = event.Find("output_index");
                if (!index || index->type != json::Value::Type::Number || index->number < 0 ||
                    std::floor(index->number) != index->number || index->number >= kMaximumToolCalls)
                    throw std::runtime_error("ChatGPT returned an invalid tool-call index");
                ToolCall call{item->GetString("call_id"), item->GetString("name"), item->GetString("arguments")};
                if (call.arguments.size() > kMaximumToolArgumentsBytes)
                    throw std::runtime_error("ChatGPT tool arguments exceed the 1 MiB safety limit");
                if (call.id.empty() || call.name.empty()) throw std::runtime_error("ChatGPT returned an incomplete tool call");
                calls[static_cast<std::size_t>(index->number)] = std::move(call);
            } else if (type == "response.completed") completed = true;
            else if (type == "response.failed" || type == "response.incomplete" || type == "error") {
                std::string message = ErrorSummary(event);
                if (message.empty())
                    if (const auto* failed = event.Find("response")) message = ErrorSummary(*failed);
                throw std::runtime_error("ChatGPT response failed" + (message.empty() ? std::string{} : ": " + message));
            }
        } catch (...) { event_error = std::current_exception(); }
    });
    struct State { SseDecoder* decoder; std::string* error; std::exception_ptr exception; } state{&decoder, &error_body, {}};
    auto write = [](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
        if (count && size > static_cast<std::size_t>(-1) / count) return 0;
        std::size_t bytes = size * count;
        auto* state = static_cast<State*>(user);
        try {
            state->decoder->Feed(data, bytes);
            if (state->error->size() < 2000)
                state->error->append(data, std::min<std::size_t>(bytes, 2000 - state->error->size()));
        } catch (...) { state->exception = std::current_exception(); return 0; }
        return bytes;
    };
    auto progress = [](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<const CancellationToken*>(user)->IsCancelled() ? 1 : 0;
    };
    std::string request_id;
    auto header = [](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
        if (count && size > static_cast<std::size_t>(-1) / count) return 0;
        const std::size_t bytes = size * count;
        std::string line(data, bytes);
        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        constexpr const char* prefix = "x-request-id:";
        if (lower.rfind(prefix, 0) == 0) {
            std::string value = line.substr(std::strlen(prefix));
            const std::size_t first = value.find_first_not_of(" \t");
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            if (first != std::string::npos && last != std::string::npos)
                *static_cast<std::string*>(user) = value.substr(first, last - first + 1);
        }
        return bytes;
    };
    CURL* raw = curl_easy_init();
    if (!raw) throw std::runtime_error("libcurl initialization failed");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);
    curl_slist* raw_headers = nullptr;
    for (const std::string& header : {std::string("Content-Type: application/json"),
                                      "Authorization: Bearer " + token}) {
        curl_slist* next = curl_slist_append(raw_headers, header.c_str());
        if (!next) {
            curl_slist_free_all(raw_headers);
            throw std::runtime_error("could not allocate ChatGPT headers");
        }
        raw_headers = next;
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, &curl_slist_free_all);
    curl_easy_setopt(raw, CURLOPT_URL, "https://api.openai.com/v1/responses");
    curl_easy_setopt(raw, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(raw, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(raw, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, +write); curl_easy_setopt(raw, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(raw, CURLOPT_HEADERFUNCTION, +header); curl_easy_setopt(raw, CURLOPT_HEADERDATA, &request_id);
    curl_easy_setopt(raw, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT, std::min<long>(30, config_.timeout_seconds));
    curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L); curl_easy_setopt(raw, CURLOPT_NETRC, CURL_NETRC_IGNORED);
    curl_easy_setopt(raw, CURLOPT_UNRESTRICTED_AUTH, 0L); curl_easy_setopt(raw, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(raw, CURLOPT_SSL_VERIFYHOST, 2L); curl_easy_setopt(raw, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(raw, CURLOPT_FORBID_REUSE, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(raw, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(raw, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    std::string certificate = CertificateFile(); if (!certificate.empty()) curl_easy_setopt(raw, CURLOPT_CAINFO, certificate.c_str());
    curl_easy_setopt(raw, CURLOPT_NOPROGRESS, 0L); curl_easy_setopt(raw, CURLOPT_XFERINFOFUNCTION, +progress);
    curl_easy_setopt(raw, CURLOPT_XFERINFODATA, const_cast<CancellationToken*>(&cancellation));
    CURLcode result = curl_easy_perform(raw);
    if (!state.exception) { try { decoder.Finish(); } catch (...) { state.exception = std::current_exception(); } }
    long status = 0; curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
    if (state.exception) std::rethrow_exception(state.exception);
    if (event_error) std::rethrow_exception(event_error);
    if (result == CURLE_ABORTED_BY_CALLBACK && cancellation.IsCancelled()) return response;
    if (result != CURLE_OK) throw std::runtime_error(std::string("ChatGPT transport failed: ") + curl_easy_strerror(result));
    if (status < 200 || status >= 300) {
        const std::string detail = ErrorSummary(error_body);
        throw std::runtime_error("ChatGPT returned HTTP " + std::to_string(status) +
            (detail.empty() ? std::string{} : ": " + detail) +
            (request_id.empty() ? std::string{} : " [request ID " + request_id + "]"));
    }
    if (!completed) throw std::runtime_error("ChatGPT stream ended before response.completed");
    for (auto& [index, call] : calls) { (void)index; response.tool_calls.push_back(std::move(call)); }
    return response;
#endif
}

}  // namespace kairo
