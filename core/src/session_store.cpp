#include "kairo/session_store.h"

#include "json.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace kairo {
namespace {

constexpr std::uintmax_t kMaximumSessionBytes = 8 * 1024 * 1024;

void WriteSecureFile(const fs::path& destination, const std::string& contents) {
    std::string pattern = destination.string() + ".tmp.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const int descriptor = ::mkstemp(writable.data());
    if (descriptor < 0) throw std::runtime_error("cannot create secure session temporary file");
    const fs::path temporary(writable.data());
    bool ok = ::fchmod(descriptor, 0600) == 0;
    std::size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t written = ::write(descriptor, contents.data() + offset, contents.size() - offset);
        if (written <= 0) ok = false;
        else offset += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(descriptor) == 0;
    if (::close(descriptor) != 0) ok = false;
    if (!ok) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw std::runtime_error("failed while saving session");
    }
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("cannot replace session file");
    }
}

json::Value ToolCallToJson(const ToolCall& call) {
    return json::Value::Object({{"id", json::Value::String(call.id)},
                                {"name", json::Value::String(call.name)},
                                {"arguments", json::Value::String(call.arguments)}});
}

json::Value MessageToJson(const Message& message) {
    std::vector<json::Value> calls;
    for (const auto& call : message.tool_calls) calls.push_back(ToolCallToJson(call));
    return json::Value::Object({{"role", json::Value::String(RoleName(message.role))},
                                {"content", json::Value::String(message.content)},
                                {"tool_call_id", json::Value::String(message.tool_call_id)},
                                {"tool_calls", json::Value::Array(std::move(calls))}});
}

ToolCall ToolCallFromJson(const json::Value& value) {
    return {value.GetString("id"), value.GetString("name"), value.GetString("arguments")};
}

Message MessageFromJson(const json::Value& value) {
    Message message;
    message.role = ParseRole(value.GetString("role"));
    message.content = value.GetString("content");
    message.tool_call_id = value.GetString("tool_call_id");
    if (const auto* calls = value.Find("tool_calls"); calls && calls->type == json::Value::Type::Array)
        for (const auto& call : calls->array) message.tool_calls.push_back(ToolCallFromJson(call));
    return message;
}

std::string NewId() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    std::mt19937_64 random(std::random_device{}());
    std::ostringstream id;
    id << std::hex << std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
       << '-' << (random() & 0xffffff);
    return id.str();
}

}  // namespace

SessionStore::SessionStore(fs::path directory) : directory_(std::move(directory)) {
    fs::create_directories(directory_);
    if (::chmod(directory_.c_str(), 0700) != 0)
        throw std::runtime_error("cannot secure session directory: " + directory_.string());
    directory_ = fs::canonical(directory_);
}

fs::path SessionStore::SessionPath(const std::string& id) const {
    if (id.empty() || id.find_first_not_of("0123456789abcdef-") != std::string::npos)
        throw std::runtime_error("invalid session id");
    return directory_ / (id + ".json");
}

Session SessionStore::Create(const fs::path& project, const std::string& model,
                             const std::string& provider) const {
    return {1, NewId(), fs::canonical(project).string(), provider, model, {}};
}

void SessionStore::Save(const Session& session) const {
    std::vector<json::Value> messages;
    for (const auto& message : session.messages) messages.push_back(MessageToJson(message));
    json::Value root = json::Value::Object({
        {"version", json::Value::Number(session.version)},
        {"id", json::Value::String(session.id)},
        {"project", json::Value::String(session.project)},
        {"provider", json::Value::String(session.provider)},
        {"model", json::Value::String(session.model)},
        {"messages", json::Value::Array(std::move(messages))},
    });
    fs::path path = SessionPath(session.id);
    WriteSecureFile(path, json::Encode(root) + '\n');
}

Session SessionStore::Load(const std::string& id) const {
    const fs::path path = SessionPath(id);
    std::error_code size_error;
    const std::uintmax_t size = fs::file_size(path, size_error);
    if (size_error) throw std::runtime_error("session not found: " + id);
    if (size > kMaximumSessionBytes) throw std::runtime_error("session file exceeds the 8 MiB safety limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("session not found: " + id);
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto root = json::Parse(contents);
    const auto* version = root.Find("version");
    if (!version || version->type != json::Value::Type::Number || static_cast<int>(version->number) != 1)
        throw std::runtime_error("unsupported session version");
    Session session;
    session.version = 1;
    session.id = root.GetString("id");
    if (session.id != id) throw std::runtime_error("session id does not match its filename");
    session.project = root.GetString("project");
    session.provider = root.GetString("provider");
    session.model = root.GetString("model");
    const auto& messages = root.At("messages");
    if (messages.type != json::Value::Type::Array)
        throw std::runtime_error("invalid session messages");
    for (const auto& message : messages.array) session.messages.push_back(MessageFromJson(message));
    return session;
}

std::vector<std::string> SessionStore::List() const {
    std::vector<std::string> ids;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") ids.push_back(entry.path().stem().string());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

}  // namespace kairo
