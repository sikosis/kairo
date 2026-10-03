#include "kairo/chatgpt_auth.h"

#include "json.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#if KAIRO_HAS_CURL
#include <curl/curl.h>
#endif
#if KAIRO_HAS_CRYPTO
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#endif

namespace fs = std::filesystem;

namespace kairo {
namespace {

constexpr const char* kDirectScope = "chatgpt.tokens.use.direct";
#if KAIRO_HAS_CURL
constexpr const char* kResource = "https://api.openai.com/v1";
constexpr const char* kTokenEndpoint = "https://auth.openai.com/api/accounts/oauth/token";
constexpr const char* kRevocationEndpoint = "https://auth.openai.com/api/accounts/oauth/revoke";
#if KAIRO_HAS_CRYPTO
constexpr const char* kIssuer = "https://auth.openai.com";
constexpr const char* kAuthorizeEndpoint = "https://auth.openai.com/api/accounts/authorize";
constexpr const char* kJwksEndpoint = "https://auth.openai.com/.well-known/jwks.json";
#endif
constexpr std::size_t kMaximumHttpBody = 4 * 1024 * 1024;
#endif

bool HasScope(const std::string& scopes, const std::string& expected) {
    std::size_t start = 0;
    while (start < scopes.size()) {
        std::size_t end = scopes.find(' ', start);
        if (scopes.substr(start, end - start) == expected) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

std::mutex& CredentialMutex() {
    static std::mutex mutex;
    return mutex;
}

#if KAIRO_HAS_CURL
long long Now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
#endif

void WriteSecureFile(const fs::path& destination, const std::string& contents) {
    fs::create_directories(destination.parent_path());
    if (::chmod(destination.parent_path().c_str(), 0700) != 0)
        throw std::runtime_error("cannot secure ChatGPT credential directory");
    std::string pattern = destination.string() + ".tmp.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    int descriptor = ::mkstemp(writable.data());
    if (descriptor < 0) throw std::runtime_error("cannot create credential temporary file");
    fs::path temporary(writable.data());
    bool ok = ::fchmod(descriptor, 0600) == 0;
    std::size_t offset = 0;
    while (ok && offset < contents.size()) {
        ssize_t written = ::write(descriptor, contents.data() + offset, contents.size() - offset);
        if (written <= 0) ok = false;
        else offset += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(descriptor) == 0;
    if (::close(descriptor) != 0) ok = false;
    std::error_code error;
    if (ok) fs::rename(temporary, destination, error);
    if (!ok || error) {
        fs::remove(temporary, error);
        throw std::runtime_error("cannot save ChatGPT credentials");
    }
}

#if KAIRO_HAS_CURL
void EnsureCurl() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("libcurl global initialization failed");
    });
}

std::string CertificateFile() {
    if (const char* value = std::getenv("CURL_CA_BUNDLE"); value && *value) return value;
    if (const char* value = std::getenv("SSL_CERT_FILE"); value && *value) return value;
#ifdef __HAIKU__
    constexpr const char* candidates[] = {
        "/boot/system/data/ssl/CARootCertificates.pem",
        "/system/data/ssl/CARootCertificates.pem",
        "/boot/home/config/non-packaged/data/ssl/CARootCertificates.pem",
    };
    for (const char* candidate : candidates) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && !error) return candidate;
    }
#endif
    return {};
}

struct HttpResult { long status = 0; std::string body; };

class OAuthError : public std::runtime_error {
public:
    OAuthError(long status, std::string code)
        : std::runtime_error("ChatGPT token exchange failed with HTTP " +
                             std::to_string(status) +
                             (code.empty() ? std::string{} : " (" + code + ")")),
          code_(std::move(code)) {}
    const std::string& Code() const { return code_; }
private:
    std::string code_;
};

HttpResult Http(const std::string& url, const std::string& method,
                const std::string& body, const std::vector<std::string>& headers,
                long timeout = 120) {
    EnsureCurl();
    CURL* raw = curl_easy_init();
    if (!raw) throw std::runtime_error("libcurl initialization failed");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, &curl_easy_cleanup);
    curl_slist* raw_headers = nullptr;
    for (const auto& header : headers) {
        curl_slist* next = curl_slist_append(raw_headers, header.c_str());
        if (!next) { curl_slist_free_all(raw_headers); throw std::runtime_error("cannot allocate HTTP headers"); }
        raw_headers = next;
    }
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> header_list(raw_headers,
                                                                            &curl_slist_free_all);
    HttpResult result;
    auto write = [](char* data, std::size_t size, std::size_t count, void* user) -> std::size_t {
        if (count && size > static_cast<std::size_t>(-1) / count) return 0;
        std::size_t bytes = size * count;
        auto* output = static_cast<std::string*>(user);
        if (bytes > kMaximumHttpBody || output->size() > kMaximumHttpBody - bytes) return 0;
        output->append(data, bytes);
        return bytes;
    };
    curl_easy_setopt(raw, CURLOPT_URL, url.c_str());
    curl_easy_setopt(raw, CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, +write);
    curl_easy_setopt(raw, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(raw, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT, std::min<long>(30, timeout));
    curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(raw, CURLOPT_NETRC, CURL_NETRC_IGNORED);
    curl_easy_setopt(raw, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(raw, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(raw, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(raw, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(raw, CURLOPT_FORBID_REUSE, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(raw, CURLOPT_PROTOCOLS_STR, "https");
#else
    curl_easy_setopt(raw, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
#endif
    const std::string certificate = CertificateFile();
    if (!certificate.empty()) curl_easy_setopt(raw, CURLOPT_CAINFO, certificate.c_str());
    if (method == "POST") {
        curl_easy_setopt(raw, CURLOPT_POST, 1L);
        curl_easy_setopt(raw, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(raw, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    CURLcode code = curl_easy_perform(raw);
    curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &result.status);
    if (code != CURLE_OK)
        throw std::runtime_error(std::string("ChatGPT HTTP transport failed: ") + curl_easy_strerror(code));
    return result;
}

std::string UrlEncode(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') encoded += c;
        else { encoded += '%'; encoded += hex[c >> 4]; encoded += hex[c & 15]; }
    }
    return encoded;
}

std::string Form(const std::map<std::string, std::string>& values) {
    std::string form;
    for (const auto& [key, value] : values) {
        if (!form.empty()) form += '&';
        form += UrlEncode(key) + "=" + UrlEncode(value);
    }
    return form;
}

#if KAIRO_HAS_CRYPTO
std::string DecodeUrl(const std::string& input) {
    std::string output;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '+') output += ' ';
        else if (input[i] == '%' && i + 2 < input.size()) {
            auto digit = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int high = digit(input[i + 1]), low = digit(input[i + 2]);
            if (high < 0 || low < 0) throw std::runtime_error("invalid OAuth callback encoding");
            output += static_cast<char>((high << 4) | low); i += 2;
        } else output += input[i];
    }
    return output;
}

std::map<std::string, std::string> Query(const std::string& query) {
    std::map<std::string, std::string> values;
    std::size_t start = 0;
    while (start <= query.size()) {
        std::size_t end = query.find('&', start);
        std::string item = query.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::size_t equals = item.find('=');
        const std::string key = DecodeUrl(item.substr(0, equals));
        const std::string value = equals == std::string::npos ? "" : DecodeUrl(item.substr(equals + 1));
        if (!values.emplace(key, value).second)
            throw std::runtime_error("duplicate field in OAuth callback");
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return values;
}

std::string Base64UrlEncode(const unsigned char* data, std::size_t size) {
    std::vector<unsigned char> output(4 * ((size + 2) / 3) + 1);
    int length = EVP_EncodeBlock(output.data(), data, static_cast<int>(size));
    std::string encoded(reinterpret_cast<char*>(output.data()), static_cast<std::size_t>(length));
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    return encoded;
}

std::vector<unsigned char> Base64UrlDecode(std::string value) {
    std::replace(value.begin(), value.end(), '-', '+');
    std::replace(value.begin(), value.end(), '_', '/');
    while (value.size() % 4) value += '=';
    std::vector<unsigned char> output(value.size() / 4 * 3 + 1);
    int length = EVP_DecodeBlock(output.data(), reinterpret_cast<const unsigned char*>(value.data()),
                                 static_cast<int>(value.size()));
    if (length < 0) throw std::runtime_error("invalid JWT base64 encoding");
    if (!value.empty() && value.back() == '=') --length;
    if (value.size() > 1 && value[value.size() - 2] == '=') --length;
    output.resize(static_cast<std::size_t>(length));
    return output;
}

std::string RandomValue(std::size_t bytes = 32) {
    std::vector<unsigned char> random(bytes);
    if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
        throw std::runtime_error("secure random generation failed");
    return Base64UrlEncode(random.data(), random.size());
}

std::string PkceChallenge(const std::string& verifier) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    if (!SHA256(reinterpret_cast<const unsigned char*>(verifier.data()), verifier.size(), digest))
        throw std::runtime_error("PKCE digest failed");
    return Base64UrlEncode(digest, sizeof(digest));
}

std::string NewHostId() {
    std::vector<unsigned char> bytes = Base64UrlDecode(RandomValue(16));
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3f) | 0x80);
    char value[64];
    std::snprintf(value, sizeof(value), "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return value;
}

struct VerifiedIdentity { std::string subject; std::string email; };

VerifiedIdentity VerifyIdToken(const std::string& token, const std::string& client_id,
                               const std::string& expected_nonce) {
    std::size_t first = token.find('.'), second = token.find('.', first == std::string::npos ? first : first + 1);
    if (first == std::string::npos || second == std::string::npos || token.find('.', second + 1) != std::string::npos)
        throw std::runtime_error("invalid ChatGPT ID token");
    std::string signed_data = token.substr(0, second);
    auto header_bytes = Base64UrlDecode(token.substr(0, first));
    auto payload_bytes = Base64UrlDecode(token.substr(first + 1, second - first - 1));
    auto signature = Base64UrlDecode(token.substr(second + 1));
    const auto header = json::Parse(std::string(header_bytes.begin(), header_bytes.end()));
    const auto payload = json::Parse(std::string(payload_bytes.begin(), payload_bytes.end()));
    if (header.GetString("alg") != "RS256") throw std::runtime_error("unsupported ChatGPT ID-token algorithm");
    const std::string kid = header.GetString("kid");
    if (kid.empty()) throw std::runtime_error("ChatGPT ID token has no key ID");
    HttpResult jwks_result = Http(kJwksEndpoint, "GET", {}, {});
    if (jwks_result.status != 200) throw std::runtime_error("could not retrieve OpenAI signing keys");
    const auto jwks = json::Parse(jwks_result.body);
    const auto* keys = jwks.Find("keys");
    if (!keys || keys->type != json::Value::Type::Array) throw std::runtime_error("invalid OpenAI signing-key response");
    const json::Value* key = nullptr;
    for (const auto& candidate : keys->array)
        if (candidate.GetString("kid") == kid && candidate.GetString("kty") == "RSA") { key = &candidate; break; }
    if (!key) throw std::runtime_error("OpenAI signing key was not found");
    auto modulus = Base64UrlDecode(key->GetString("n"));
    auto exponent = Base64UrlDecode(key->GetString("e"));
    std::unique_ptr<BIGNUM, decltype(&BN_free)> n(
        BN_bin2bn(modulus.data(), static_cast<int>(modulus.size()), nullptr), &BN_free);
    std::unique_ptr<BIGNUM, decltype(&BN_free)> e(
        BN_bin2bn(exponent.data(), static_cast<int>(exponent.size()), nullptr), &BN_free);
    std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> parameters(
        OSSL_PARAM_BLD_new(), &OSSL_PARAM_BLD_free);
    if (!n || !e || !parameters ||
        OSSL_PARAM_BLD_push_BN(parameters.get(), OSSL_PKEY_PARAM_RSA_N, n.get()) != 1 ||
        OSSL_PARAM_BLD_push_BN(parameters.get(), OSSL_PKEY_PARAM_RSA_E, e.get()) != 1)
        throw std::runtime_error("could not construct OpenAI signing key");
    std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> key_parameters(
        OSSL_PARAM_BLD_to_param(parameters.get()), &OSSL_PARAM_free);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
        EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), &EVP_PKEY_CTX_free);
    EVP_PKEY* raw_key = nullptr;
    if (!key_parameters || !key_context || EVP_PKEY_fromdata_init(key_context.get()) != 1 ||
        EVP_PKEY_fromdata(key_context.get(), &raw_key, EVP_PKEY_PUBLIC_KEY,
                          key_parameters.get()) != 1 || !raw_key)
        throw std::runtime_error("could not prepare OpenAI signing key");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> public_key(raw_key, &EVP_PKEY_free);
    EVP_MD_CTX* raw_context = EVP_MD_CTX_new();
    if (!raw_context) throw std::runtime_error("could not create signature verifier");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw_context, &EVP_MD_CTX_free);
    if (EVP_DigestVerifyInit(raw_context, nullptr, EVP_sha256(), nullptr, raw_key) != 1 ||
        EVP_DigestVerifyUpdate(raw_context, signed_data.data(), signed_data.size()) != 1 ||
        EVP_DigestVerifyFinal(raw_context, signature.data(), signature.size()) != 1)
        throw std::runtime_error("ChatGPT ID-token signature is invalid");
    if (payload.GetString("iss") != kIssuer) throw std::runtime_error("ChatGPT ID-token issuer is invalid");
    bool audience = payload.GetString("aud") == client_id;
    if (const auto* aud = payload.Find("aud"); aud && aud->type == json::Value::Type::Array)
        for (const auto& item : aud->array) audience |= item.type == json::Value::Type::String && item.string == client_id;
    if (!audience) throw std::runtime_error("ChatGPT ID-token audience is invalid");
    const auto* expiry = payload.Find("exp");
    const auto* issued = payload.Find("iat");
    const double now = static_cast<double>(Now());
    if (!expiry || expiry->type != json::Value::Type::Number || !std::isfinite(expiry->number) ||
        expiry->number < now - 5 || !issued || issued->type != json::Value::Type::Number ||
        !std::isfinite(issued->number) || issued->number > now + 300)
        throw std::runtime_error("ChatGPT ID token is expired or incomplete");
    if (const auto* aud = payload.Find("aud"); aud && aud->type == json::Value::Type::Array &&
        aud->array.size() > 1 && payload.GetString("azp") != client_id)
        throw std::runtime_error("ChatGPT ID-token authorized party is invalid");
    if (payload.GetString("nonce") != expected_nonce) throw std::runtime_error("ChatGPT ID-token nonce is invalid");
    VerifiedIdentity identity{payload.GetString("sub"), payload.GetString("email")};
    if (identity.subject.empty()) throw std::runtime_error("ChatGPT ID token has no subject");
    return identity;
}
#endif

json::Value TokenExchange(const std::map<std::string, std::string>& fields) {
    HttpResult result = Http(kTokenEndpoint, "POST", Form(fields),
                             {"Content-Type: application/x-www-form-urlencoded"});
    if (result.status != 200) {
        std::string code;
        try {
            const auto error = json::Parse(result.body);
            code = error.GetString("error");
            if (code.empty())
                if (const auto* nested = error.Find("error")) code = nested->GetString("code");
        } catch (...) {}
        throw OAuthError(result.status, std::move(code));
    }
    return json::Parse(result.body);
}

bool IsUnusableRefreshCode(const std::string& code) {
    return code == "invalid_grant" || code == "invalid_refresh_token" ||
           code == "token_expired" || code == "refresh_token_expired" ||
           code == "refresh_token_invalidated" || code == "refresh_token_reused";
}
#endif

}  // namespace

ChatGPTAuth::ChatGPTAuth(fs::path credential_file) : credential_file_(std::move(credential_file)) {}

ChatGPTAccount ChatGPTAuth::Load() const {
    ChatGPTAccount account;
    struct stat metadata{};
    if (::lstat(credential_file_.c_str(), &metadata) != 0) {
        if (errno == ENOENT) return account;
        throw std::runtime_error("cannot inspect ChatGPT credential file");
    }
    if (!S_ISREG(metadata.st_mode))
        throw std::runtime_error("ChatGPT credential path is not a regular file");
    if ((metadata.st_mode & 077) != 0 && ::chmod(credential_file_.c_str(), 0600) != 0)
        throw std::runtime_error("cannot secure ChatGPT credential file");
    std::ifstream input(credential_file_, std::ios::binary);
    if (!input) return account;
    std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (contents.size() > 1024 * 1024) throw std::runtime_error("ChatGPT credential file is too large");
    const auto root = json::Parse(contents);
    account.email = root.GetString("email"); account.subject = root.GetString("subject");
    account.client_id = root.GetString("client_id"); account.host_id = root.GetString("host_id");
    account.id_token = root.GetString("id_token"); account.access_token = root.GetString("access_token");
    account.refresh_token = root.GetString("refresh_token"); account.scope = root.GetString("scope");
    if (const auto* expiry = root.Find("expires_at"); expiry && expiry->type == json::Value::Type::Number)
        account.expires_at = static_cast<long long>(expiry->number);
    return account;
}

void ChatGPTAuth::Save(const ChatGPTAccount& account) const {
    json::Value root = json::Value::Object({
        {"version", json::Value::Number(1)}, {"email", json::Value::String(account.email)},
        {"subject", json::Value::String(account.subject)}, {"client_id", json::Value::String(account.client_id)},
        {"host_id", json::Value::String(account.host_id)}, {"id_token", json::Value::String(account.id_token)},
        {"access_token", json::Value::String(account.access_token)},
        {"refresh_token", json::Value::String(account.refresh_token)},
        {"scope", json::Value::String(account.scope)},
        {"expires_at", json::Value::Number(static_cast<double>(account.expires_at))},
    });
    WriteSecureFile(credential_file_, json::Encode(root) + '\n');
}

bool ChatGPTAuth::IsSignedIn() const {
    const auto account = Load();
    return !account.client_id.empty() && !account.subject.empty() &&
           !account.access_token.empty() && !account.refresh_token.empty() && HasScope(account.scope, kDirectScope);
}

ChatGPTAccount ChatGPTAuth::Account() const { return Load(); }

ChatGPTAccount ChatGPTAuth::SignIn(const BrowserLauncher& launch_browser,
                                   const CancellationToken& cancellation) {
#if !KAIRO_HAS_CURL
    (void)launch_browser; (void)cancellation;
    throw std::runtime_error("Kairo was built without ChatGPT network support");
#elif !KAIRO_HAS_CRYPTO
    (void)launch_browser; (void)cancellation;
    throw std::runtime_error("Kairo was built without OpenSSL support required for secure ChatGPT sign-in");
#else
    std::lock_guard<std::mutex> credential_lock(CredentialMutex());
    ChatGPTAccount previous = Load();
    if (previous.host_id.empty()) {
        previous.host_id = NewHostId();
        Save(previous);
    }
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) throw std::runtime_error("cannot create ChatGPT callback listener");
    struct SocketCloser { void operator()(int* fd) const { if (fd) { ::close(*fd); delete fd; } } };
    std::unique_ptr<int, SocketCloser> socket_guard(new int(listener));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(listener, 1) != 0)
        throw std::runtime_error("cannot start ChatGPT callback listener");
    socklen_t address_size = sizeof(address);
    if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0)
        throw std::runtime_error("cannot determine ChatGPT callback port");
    const std::string redirect = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/auth/callback";
    const std::string state = RandomValue(), nonce = RandomValue(), verifier = RandomValue(64);
    const std::string request_client = previous.client_id.empty() ? "dynamic_agent_client" : previous.client_id;
    std::map<std::string, std::string> parameters{
        {"client_id", request_client}, {"response_type", "code"}, {"redirect_uri", redirect},
        {"scope", "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"},
        {"resource", kResource}, {"state", state}, {"nonce", nonce},
        {"code_challenge_method", "S256"}, {"code_challenge", PkceChallenge(verifier)},
        {"ext_agent_host_id", previous.host_id},
    };
    if (previous.client_id.empty()) parameters["agent_name_hint"] = "Kairo";
    else {
        if (!previous.id_token.empty()) parameters["id_token_hint"] = previous.id_token;
        if (!previous.email.empty()) parameters["login_hint"] = previous.email;
    }
    launch_browser(std::string(kAuthorizeEndpoint) + "?" + Form(parameters));
    auto send_reply = [](int client, const char* status, const std::string& html) {
        const std::string reply = std::string("HTTP/1.1 ") + status +
            "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
            std::to_string(html.size()) + "\r\n\r\n" + html;
        const char* data = reply.data();
        std::size_t remaining = reply.size();
        while (remaining > 0) {
            const ssize_t written = ::write(client, data, remaining);
            if (written <= 0) break;
            data += written;
            remaining -= static_cast<std::size_t>(written);
        }
    };
    std::map<std::string, std::string> values;
    bool callback_received = false;
    constexpr int kSignInTimeoutSeconds = 900;
    for (int elapsed = 0; elapsed < kSignInTimeoutSeconds && !callback_received; ++elapsed) {
        if (cancellation.IsCancelled()) throw std::runtime_error("ChatGPT sign-in was cancelled");
        fd_set set; FD_ZERO(&set); FD_SET(listener, &set); timeval timeout{1, 0};
        int ready = ::select(listener + 1, &set, nullptr, nullptr, &timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("ChatGPT callback listener failed");
        }
        if (ready == 0) continue;
        int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("ChatGPT callback accept failed");
        }
        std::unique_ptr<int, SocketCloser> client_guard(new int(client));
        timeval receive_timeout{5, 0};
        if (::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
                         sizeof(receive_timeout)) != 0)
            continue;
        std::string request;
        char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            ssize_t count = ::read(client, buffer, sizeof(buffer));
            if (count <= 0) break;
            request.append(buffer, static_cast<std::size_t>(count));
        }
        std::size_t line_end = request.find("\r\n");
        if (request.rfind("GET ", 0) != 0 || line_end == std::string::npos) {
            send_reply(client, "400 Bad Request",
                "<!doctype html><title>Kairo</title><p>Invalid Kairo callback request.</p>");
            continue;
        }
        std::string target = request.substr(4, line_end - 4);
        std::size_t space = target.find(' ');
        if (space != std::string::npos) target.resize(space);
        std::size_t question = target.find('?');
        if (question == std::string::npos || target.substr(0, question) != "/auth/callback") {
            send_reply(client, "404 Not Found",
                "<!doctype html><title>Kairo</title><p>This is not a Kairo authorization callback.</p>");
            continue;
        }
        try {
            values = Query(target.substr(question + 1));
        } catch (const std::exception&) {
            send_reply(client, "400 Bad Request",
                "<!doctype html><title>Kairo</title><p>The Kairo callback was malformed.</p>");
            continue;
        }
        const std::string returned_state = values["state"];
        if (returned_state.size() != state.size() ||
            CRYPTO_memcmp(returned_state.data(), state.data(), state.size()) != 0) {
            values.clear();
            send_reply(client, "400 Bad Request",
                "<!doctype html><title>Kairo</title><p>This callback does not belong to the active Kairo sign-in.</p>");
            continue;
        }
        send_reply(client, "200 OK",
            "<!doctype html><title>Kairo</title><p>Authorization received. You can return to Kairo.</p>");
        callback_received = true;
    }
    if (!callback_received)
        throw std::runtime_error("ChatGPT sign-in timed out before the browser returned to Kairo; if WebPositive showed security or Cloudflare errors, retry the copied sign-in URL in Firefox");
    if (!values["error"].empty()) throw std::runtime_error("ChatGPT sign-in was denied: " + values["error"]);
    if (values["code"].empty()) throw std::runtime_error("ChatGPT callback did not contain a code");
    std::string client_id = previous.client_id;
    if (client_id.empty()) client_id = values["client_id"];
    else if (!values["client_id"].empty() && values["client_id"] != client_id)
        throw std::runtime_error("ChatGPT callback returned a different client registration");
    if (client_id.empty() || client_id == "dynamic_agent_client")
        throw std::runtime_error("ChatGPT registration did not return an issued client ID");
    auto tokens = TokenExchange({{"grant_type", "authorization_code"}, {"client_id", client_id},
        {"code", values["code"]}, {"code_verifier", verifier}, {"redirect_uri", redirect}, {"resource", kResource}});
    std::string id_token = tokens.GetString("id_token");
    VerifiedIdentity identity = VerifyIdToken(id_token, client_id, nonce);
    if (!previous.subject.empty() && previous.subject != identity.subject)
        throw std::runtime_error("ChatGPT sign-in returned a different account");
    ChatGPTAccount account;
    account.email = identity.email; account.subject = identity.subject; account.client_id = client_id;
    account.host_id = previous.host_id; account.id_token = id_token;
    account.access_token = tokens.GetString("access_token"); account.refresh_token = tokens.GetString("refresh_token");
    account.scope = tokens.GetString("scope");
    const auto* expires = tokens.Find("expires_in");
    account.expires_at = Now() + (expires && expires->type == json::Value::Type::Number
        ? static_cast<long long>(expires->number) : 3600);
    if (account.access_token.empty() || account.refresh_token.empty())
        throw std::runtime_error("ChatGPT sign-in returned incomplete credentials");
    Save(account);
    if (!HasScope(account.scope, kDirectScope))
        throw std::runtime_error("ChatGPT account connected, but ChatGPT plan access was not granted; use Reconnect to enable it");
    return account;
#endif
}

std::string ChatGPTAuth::AccessToken() {
#if !KAIRO_HAS_CURL
    throw std::runtime_error("Kairo was built without ChatGPT network support");
#else
    std::lock_guard<std::mutex> credential_lock(CredentialMutex());
    ChatGPTAccount account = Load();
    if (account.client_id.empty() || account.subject.empty() || account.access_token.empty() ||
        account.refresh_token.empty() || !HasScope(account.scope, kDirectScope))
        throw std::runtime_error("Sign in with ChatGPT in Provider Settings first");
    if (account.expires_at > Now() + 90) return account.access_token;
    json::Value tokens;
    try {
        tokens = TokenExchange({{"grant_type", "refresh_token"}, {"client_id", account.client_id},
            {"refresh_token", account.refresh_token}, {"resource", kResource}});
    } catch (const OAuthError& error) {
        if (IsUnusableRefreshCode(error.Code())) {
            account.access_token.clear(); account.refresh_token.clear();
            account.scope.clear(); account.expires_at = 0;
            Save(account);
        }
        throw;
    }
    const std::string access = tokens.GetString("access_token");
    const std::string refresh = tokens.GetString("refresh_token");
    if (access.empty() || refresh.empty()) throw std::runtime_error("ChatGPT token refresh returned incomplete credentials");
    account.access_token = access; account.refresh_token = refresh;
    if (!tokens.GetString("scope").empty()) account.scope = tokens.GetString("scope");
    const auto* expires = tokens.Find("expires_in");
    account.expires_at = Now() + (expires && expires->type == json::Value::Type::Number
        ? static_cast<long long>(expires->number) : 3600);
    Save(account);
    return account.access_token;
#endif
}

std::vector<ChatGPTModel> ChatGPTAuth::ListModels() {
#if !KAIRO_HAS_CURL
    throw std::runtime_error("Kairo was built without ChatGPT network support");
#else
    const std::string token = AccessToken();
    HttpResult result = Http("https://api.openai.com/v1/models", "GET", {},
                             {"Authorization: Bearer " + token});
    if (result.status != 200) throw std::runtime_error("ChatGPT model discovery failed with HTTP " + std::to_string(result.status));
    const auto root = json::Parse(result.body);
    const auto* models = root.Find("models");
    if (!models || models->type != json::Value::Type::Array) throw std::runtime_error("invalid ChatGPT model response");
    std::vector<ChatGPTModel> output;
    for (const auto& item : models->array) {
        if (item.GetString("visibility") != "list") continue;
        ChatGPTModel model{item.GetString("slug"), item.GetString("display_name")};
        if (!model.slug.empty()) { if (model.display_name.empty()) model.display_name = model.slug; output.push_back(std::move(model)); }
    }
    if (output.empty()) throw std::runtime_error("No ChatGPT-plan models are available to this account");
    return output;
#endif
}

void ChatGPTAuth::SignOut() {
    std::lock_guard<std::mutex> credential_lock(CredentialMutex());
    ChatGPTAccount account = Load();
    if (account.client_id.empty()) return;
    std::string error;
#if KAIRO_HAS_CURL
    if (!account.refresh_token.empty()) {
        try {
            HttpResult result = Http(kRevocationEndpoint, "POST", Form({{"token", account.refresh_token},
                {"token_type_hint", "refresh_token"}, {"client_id", account.client_id}}),
                {"Content-Type: application/x-www-form-urlencoded"});
            if (result.status != 200) error = "OpenAI did not confirm remote sign-out";
        } catch (const std::exception& exception) { error = exception.what(); }
    }
#endif
    account.id_token.clear(); account.access_token.clear(); account.refresh_token.clear();
    account.scope.clear(); account.expires_at = 0;
    Save(account);
    if (!error.empty()) throw std::runtime_error(error + "; local credentials were cleared");
}

}  // namespace kairo
