#include "kairo/openai_provider.h"

#include "json.h"
#include "kairo/sse_decoder.h"

#include <cstdlib>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>

#if KAIRO_HAS_CURL
#include <curl/curl.h>
#endif

namespace kairo {
namespace {

std::string Lowercase(std::string value) {
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}

void ValidateBaseUrl(const std::string& url) {
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
        throw std::invalid_argument("provider URL must start with https://");
    const std::string scheme = Lowercase(url.substr(0, scheme_end));
    const std::size_t authority_start = scheme_end + 3;
    const std::size_t authority_end = url.find('/', authority_start);
    const std::string authority = url.substr(authority_start, authority_end - authority_start);
    if (authority.empty() || authority.find('@') != std::string::npos ||
        url.find_first_of("?#") != std::string::npos)
        throw std::invalid_argument("provider URL contains an invalid authority, query, or fragment");

    std::string host = authority;
    if (!host.empty() && host.front() == '[') {
        const std::size_t closing = host.find(']');
        if (closing == std::string::npos)
            throw std::invalid_argument("provider URL contains an invalid IPv6 address");
        host = host.substr(1, closing - 1);
    } else {
        const std::size_t colon = host.find(':');
        if (colon != std::string::npos) host.resize(colon);
    }
    host = Lowercase(host);
    if (host.empty()) throw std::invalid_argument("provider URL must include a host");
    const bool loopback = host == "localhost" || host == "127.0.0.1" || host == "::1";
    if (scheme != "https" && !(scheme == "http" && loopback))
        throw std::invalid_argument("provider URL must use HTTPS (HTTP is allowed only for localhost)");
}

#if KAIRO_HAS_CURL
std::string Redact(std::string value, const std::string& secret) {
    if (secret.empty()) return value;
    std::size_t found = 0;
    while ((found = value.find(secret, found)) != std::string::npos) {
        value.replace(found, secret.size(), "[redacted]");
        found += 10;
    }
    return value;
}
#endif

#if KAIRO_HAS_CURL
constexpr std::size_t kMaximumResponseTextBytes = 4 * 1024 * 1024;
constexpr std::size_t kMaximumToolArgumentsBytes = 1024 * 1024;
constexpr std::size_t kMaximumToolCalls = 128;

struct CertificateLocations {
    std::string file;
    std::string directory;
};

CertificateLocations FindCertificateLocations() {
    CertificateLocations locations;
    if (const char* value = std::getenv("CURL_CA_BUNDLE"); value && *value)
        locations.file = value;
    else if (const char* value = std::getenv("SSL_CERT_FILE"); value && *value)
        locations.file = value;
    if (const char* value = std::getenv("SSL_CERT_DIR"); value && *value)
        locations.directory = value;

#ifdef __HAIKU__
    if (locations.file.empty()) {
        constexpr const char* candidates[] = {
            "/boot/system/data/ssl/CARootCertificates.pem",
            "/system/data/ssl/CARootCertificates.pem",
            "/boot/home/config/non-packaged/data/ssl/CARootCertificates.pem",
            "/boot/system/non-packaged/data/ssl/CARootCertificates.pem",
        };
        for (const char* candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                locations.file = candidate;
                break;
            }
        }
    }
#endif
    return locations;
}

json::Value ToolDefinition(const std::string& name, const std::string& description,
                           std::map<std::string, json::Value> properties,
                           std::vector<json::Value> required) {
    return json::Value::Object({
        {"type", json::Value::String("function")},
        {"function", json::Value::Object({
            {"name", json::Value::String(name)},
            {"description", json::Value::String(description)},
            {"parameters", json::Value::Object({
                {"type", json::Value::String("object")},
                {"properties", json::Value::Object(std::move(properties))},
                {"required", json::Value::Array(std::move(required))},
                {"additionalProperties", json::Value::Boolean(false)},
            })},
        })},
    });
}

json::Value StringProperty(const std::string& description) {
    return json::Value::Object({{"type", json::Value::String("string")},
                                {"description", json::Value::String(description)}});
}

json::Value MessageJson(const Message& message) {
    std::map<std::string, json::Value> object{{"role", json::Value::String(RoleName(message.role))},
                                              {"content", json::Value::String(message.content)}};
    if (!message.tool_call_id.empty()) object["tool_call_id"] = json::Value::String(message.tool_call_id);
    if (!message.tool_calls.empty()) {
        std::vector<json::Value> calls;
        for (const auto& call : message.tool_calls) {
            calls.push_back(json::Value::Object({
                {"id", json::Value::String(call.id)},
                {"type", json::Value::String("function")},
                {"function", json::Value::Object({{"name", json::Value::String(call.name)},
                                                   {"arguments", json::Value::String(call.arguments)}})},
            }));
        }
        object["tool_calls"] = json::Value::Array(std::move(calls));
    }
    return json::Value::Object(std::move(object));
}

json::Value RequestJson(const ProviderRequest& request) {
    std::vector<json::Value> messages;
    for (const auto& message : request.messages) messages.push_back(MessageJson(message));
    auto required = [](const std::string& value) { return std::vector<json::Value>{json::Value::String(value)}; };
    std::vector<json::Value> tools;
    tools.push_back(ToolDefinition("read_file", "Read a UTF-8 file inside the selected project.",
        {{"path", StringProperty("Literal project-relative file path; do not use '~' or environment variables")}}, required("path")));
    tools.push_back(ToolDefinition("search_files", "Search text in project files.",
        {{"query", StringProperty("Literal search text")}, {"path", StringProperty("Optional literal project-relative directory; use '.' for the project root")}}, required("query")));
    tools.push_back(ToolDefinition("write_file", "Write a project file after user approval.",
        {{"path", StringProperty("Project-relative path")}, {"content", StringProperty("Complete replacement content")}},
        {json::Value::String("path"), json::Value::String("content")}));
    tools.push_back(ToolDefinition("run_shell", "Run a shell command after user approval.",
        {{"command", StringProperty("Exact shell command")}, {"working_directory", StringProperty("Literal project-relative directory; use '.' for the project root")}},
        {json::Value::String("command")}));
    return json::Value::Object({{"model", json::Value::String(request.model)},
                                {"messages", json::Value::Array(std::move(messages))},
                                {"tools", json::Value::Array(std::move(tools))},
                                {"stream", json::Value::Boolean(true)}});
}
#endif

}  // namespace

OpenAIProvider::OpenAIProvider(OpenAIConfig config) : config_(std::move(config)) {
    ValidateBaseUrl(config_.base_url);
    if (config_.timeout_seconds <= 0)
        throw std::invalid_argument("provider timeout must be positive");
}

ProviderResponse OpenAIProvider::Complete(const ProviderRequest& request, const EventSink& events,
                                          const CancellationToken& cancellation) {
#if !KAIRO_HAS_CURL
    (void)request; (void)events; (void)cancellation;
    throw std::runtime_error("Kairo was built without libcurl; the OpenAI-compatible provider is unavailable");
#else
    static std::once_flag curl_initialized;
    std::call_once(curl_initialized, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("libcurl global initialization failed");
    });
    std::string key = config_.api_key;
    if (key.empty()) {
        const char* key_value = std::getenv(config_.api_key_environment.c_str());
        if (key_value && *key_value) key = key_value;
    }
    if (key.empty())
        throw std::runtime_error("API key is not configured in the GUI or environment variable: "
                                 + config_.api_key_environment);
    const std::string body = json::Encode(RequestJson(request));
    ProviderResponse response;
    std::map<std::size_t, ToolCall> calls;
    std::string error_body;
    std::exception_ptr callback_error;

    SseDecoder decoder([&](const std::string& data) {
        if (data == "[DONE]" || callback_error) return;
        try {
            const auto event = json::Parse(data);
            if (const auto* error = event.Find("error"))
                throw std::runtime_error("provider stream error: " + error->GetString("message", "unknown error"));
            const auto* choices = event.Find("choices");
            if (!choices || choices->type != json::Value::Type::Array || choices->array.empty()) return;
            const auto* delta = choices->array[0].Find("delta");
            if (!delta) return;
            if (const auto* content = delta->Find("content"); content && content->type == json::Value::Type::String) {
                if (content->string.size() > kMaximumResponseTextBytes -
                                             std::min(response.text.size(), kMaximumResponseTextBytes))
                    throw std::runtime_error("provider response text exceeds the 4 MiB safety limit");
                response.text += content->string;
                if (events) events({EventType::TextDelta, {}, {}, content->string});
            }
            const auto* chunks = delta->Find("tool_calls");
            if (!chunks || chunks->type != json::Value::Type::Array) return;
            for (const auto& chunk : chunks->array) {
                const auto* index = chunk.Find("index");
                std::size_t i = 0;
                if (index && index->type == json::Value::Type::Number) {
                    if (index->number < 0 || std::floor(index->number) != index->number ||
                        index->number >= static_cast<double>(kMaximumToolCalls))
                        throw std::runtime_error("provider returned an invalid tool-call index");
                    i = static_cast<std::size_t>(index->number);
                }
                if (i >= kMaximumToolCalls)
                    throw std::runtime_error("provider returned too many tool calls");
                ToolCall& call = calls[i];
                std::string id = chunk.GetString("id");
                if (!id.empty()) call.id = std::move(id);
                if (const auto* function = chunk.Find("function")) {
                    std::string name = function->GetString("name");
                    if (!name.empty()) call.name = std::move(name);
                    std::string arguments = function->GetString("arguments");
                    if (arguments.size() > kMaximumToolArgumentsBytes -
                                           std::min(call.arguments.size(), kMaximumToolArgumentsBytes))
                        throw std::runtime_error("tool arguments exceed the 1 MiB safety limit");
                    call.arguments += arguments;
                }
            }
        } catch (...) { callback_error = std::current_exception(); }
    });

    struct CallbackState {
        SseDecoder* decoder;
        std::string* error;
        std::exception_ptr exception;
    } callback_state{&decoder, &error_body, {}};
    auto write_callback = [](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
        auto* state = static_cast<CallbackState*>(user);
        if (count != 0 && size > static_cast<std::size_t>(-1) / count) return 0;
        std::size_t bytes = size * count;
        try {
            state->decoder->Feed(data, bytes);
            if (state->error->size() < 2000)
                state->error->append(data, std::min<std::size_t>(bytes, 2000 - state->error->size()));
        } catch (...) {
            state->exception = std::current_exception();
            return 0;
        }
        return bytes;
    };
    auto progress_callback = [](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return static_cast<CancellationToken*>(user)->IsCancelled() ? 1 : 0;
    };

    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("libcurl initialization failed");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl, &curl_easy_cleanup);
    std::string url = config_.base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";
    struct curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    if (!raw_headers) throw std::runtime_error("could not allocate HTTP headers");
    struct curl_slist* with_authorization =
        curl_slist_append(raw_headers, ("Authorization: Bearer " + key).c_str());
    if (!with_authorization) {
        curl_slist_free_all(raw_headers);
        throw std::runtime_error("could not allocate HTTP headers");
    }
    raw_headers = with_authorization;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, &curl_slist_free_all);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callback_state);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, config_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min<long>(30, config_.timeout_seconds));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NETRC, CURL_NETRC_IGNORED) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L) != CURLE_OK)
        throw std::runtime_error("libcurl could not apply the restricted HTTP configuration");
    if (curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK)
        throw std::runtime_error("libcurl could not enable TLS certificate verification");
#if LIBCURL_VERSION_NUM >= 0x075500
    if (curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https") != CURLE_OK)
        throw std::runtime_error("libcurl could not restrict provider URL protocols");
#else
    if (curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS) != CURLE_OK)
        throw std::runtime_error("libcurl could not restrict provider URL protocols");
#endif
    const CertificateLocations certificates = FindCertificateLocations();
    if (!certificates.file.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, certificates.file.c_str());
    if (!certificates.directory.empty())
        curl_easy_setopt(curl, CURLOPT_CAPATH, certificates.directory.c_str());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<CancellationToken*>(&cancellation));
    CURLcode result = curl_easy_perform(curl);
    if (!callback_state.exception) {
        try { decoder.Finish(); }
        catch (...) { callback_state.exception = std::current_exception(); }
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    auto rethrow_sanitized = [&](const std::exception_ptr& exception) {
        try { std::rethrow_exception(exception); }
        catch (const std::exception& error) {
            throw std::runtime_error(Redact(error.what(), key));
        } catch (...) {
            throw std::runtime_error("provider response processing failed");
        }
    };
    if (callback_state.exception) rethrow_sanitized(callback_state.exception);
    if (callback_error) rethrow_sanitized(callback_error);
    if (result == CURLE_ABORTED_BY_CALLBACK && cancellation.IsCancelled()) return response;
    if (result == CURLE_SSL_CACERT_BADFILE) {
        if (certificates.file.empty())
            throw std::runtime_error("HTTP transport failed: no readable CA bundle was found; "
                                     "install the Haiku ca_root_certificates package");
        throw std::runtime_error("HTTP transport failed: libcurl could not read the configured CA bundle: "
                                 + certificates.file);
    }
    if (result != CURLE_OK) throw std::runtime_error(std::string("HTTP transport failed: ") + curl_easy_strerror(result));
    if (status < 200 || status >= 300) {
        throw std::runtime_error("provider returned HTTP " + std::to_string(status) + ": " +
                                 Redact(error_body, key));
    }
    for (auto& [index, call] : calls) {
        (void)index;
        if (call.id.empty()) call.id = "call_" + std::to_string(response.tool_calls.size());
        response.tool_calls.push_back(std::move(call));
    }
    return response;
#endif
}

}  // namespace kairo
