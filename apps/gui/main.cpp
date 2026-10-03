#ifndef __HAIKU__
#error "The native Kairo GUI must be compiled on Haiku."
#endif

#include "kairo/agent_engine.h"
#include "kairo/chatgpt_auth.h"
#include "kairo/diagnostics.h"
#include "kairo/openai_provider.h"
#include "kairo/provider_profile.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <CheckBox.h>
#include <Clipboard.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Font.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <NodeInfo.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <ScrollView.h>
#include <Size.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <TypeConstants.h>
#include <Window.h>

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr uint32 kSend = 'send';
constexpr uint32 kCancel = 'cncl';
constexpr uint32 kChooseProject = 'chpr';
constexpr uint32 kNewSession = 'news';
constexpr uint32 kClearTranscript = 'cltr';
constexpr uint32 kFocusPrompt = 'fcpr';
constexpr uint32 kQuit = 'quit';
constexpr uint32 kOpenSettings = 'opst';
constexpr uint32 kSaveSettings = 'svst';
constexpr uint32 kActivateSettings = 'stac';
constexpr uint32 kApprovalModeChanged = 'apmd';
constexpr uint32 kEngineEvent = 'kevt';
constexpr uint32 kApprovalRequest = 'kapr';
constexpr uint32 kApprovalDecision = 'kapd';
constexpr uint32 kProviderChanged = 'pvch';
constexpr uint32 kModelChanged = 'mdch';
constexpr uint32 kSettingsProviderChanged = 'spch';
constexpr uint32 kSettingsKindChanged = 'skch';
constexpr uint32 kSettingsAddProvider = 'sadd';
constexpr uint32 kSettingsDeleteProvider = 'sdel';
constexpr uint32 kChatGPTSignIn = 'cgsi';
constexpr uint32 kChatGPTSignOut = 'cgso';
constexpr uint32 kChatGPTAuthResult = 'cgar';
constexpr uint32 kOpenAuthUrl = 'cgau';
constexpr const char* kApplicationSignature = "application/x-vnd.Kairo-Agent";

fs::path ChatGPTCredentialPath();

std::string ApplicationVersion() {
//---------------------------------------------------------------------------------------------------------------------------------//

    return KAIRO_VERSION;
}

BBitmap* ApplicationIcon() {
//---------------------------------------------------------------------------------------------------------------------------------//

    app_info info;
    if (be_app->GetAppInfo(&info) != B_OK) return nullptr;

    BBitmap* icon = new BBitmap(BRect(0, 0, 31, 31), B_RGBA32);
    if (icon->InitCheck() != B_OK ||
        BNodeInfo::GetTrackerIcon(&info.ref, icon, B_LARGE_ICON) != B_OK) {
        delete icon;
        return nullptr;
    }
    return icon;
}

bool ProfileHasKey(const kairo::ProviderProfile& profile) {
    if (!kairo::ProviderUsesApiKey(profile.kind)) return true;
    if (!profile.api_key.empty()) return true;
    const char* key = profile.api_key_environment.empty()
        ? nullptr : std::getenv(profile.api_key_environment.c_str());
    return key && *key;
}

std::string ReadyStatus(const kairo::ProviderProfile& profile) {
    if (kairo::ProviderUsesChatGPTPlan(profile.kind)) {
        try {
            kairo::ChatGPTAuth auth(ChatGPTCredentialPath());
            return auth.IsSignedIn() ? "Ready - ChatGPT account connected"
                                     : "ChatGPT sign-in required";
        } catch (...) { return "ChatGPT sign-in required"; }
    }
    return ProfileHasKey(profile)
        ? "Ready - " + profile.name + " key configured"
        : "Ready - " + profile.name + " key missing";
}

struct PendingApproval {
    std::mutex mutex;
    std::condition_variable changed;
    bool resolved = false;
    bool approved = false;

    bool Wait(const kairo::CancellationToken& cancellation) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!resolved && !cancellation.IsCancelled())
            changed.wait_for(lock, std::chrono::milliseconds(100));
        return resolved && approved;
    }
    void Resolve(bool value) {
        std::lock_guard<std::mutex> lock(mutex);
        if (resolved) return;
        resolved = true;
        approved = value;
        changed.notify_all();
    }
};

fs::path SessionDirectory() {
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
        return fs::temp_directory_path() / "Kairo" / "sessions";
    return fs::path(path.Path()) / "Kairo" / "sessions";
}

fs::path ChatGPTCredentialPath() {
    return SessionDirectory().parent_path() / "chatgpt_credentials.json";
}

fs::path LogPath() {
    return SessionDirectory().parent_path() / "kairo.log";
}

void WriteLog(const char* level, const std::string& message) noexcept {
    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    try {
        fs::path path = LogPath();
        std::error_code error;
        fs::create_directories(path.parent_path(), error);
        if (error || chmod(path.parent_path().c_str(), S_IRWXU) != 0) return;

        int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND,
                              S_IRUSR | S_IWUSR);
        if (descriptor < 0) return;
        if (fchmod(descriptor, S_IRUSR | S_IWUSR) != 0) {
            close(descriptor);
            return;
        }

        std::time_t now = std::time(nullptr);
        std::tm local_time{};
        char timestamp[32] = "unknown-time";
        if (localtime_r(&now, &local_time))
            std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local_time);
        std::string line = std::string(timestamp) + " [" + level + "] "
            + kairo::SanitiseDiagnostic(message) + "\n";
        const char* data = line.data();
        size_t remaining = line.size();
        while (remaining > 0) {
            ssize_t written = write(descriptor, data, remaining);
            if (written <= 0) break;
            data += written;
            remaining -= static_cast<size_t>(written);
        }
        close(descriptor);
    } catch (...) {
        // Logging must never take down the GUI.
    }
}

bool CopyTextToClipboard(const std::string& text) {
//---------------------------------------------------------------------------------------------------------------------------------//

    if (!be_clipboard || !be_clipboard->Lock()) return false;
    be_clipboard->Clear();
    BMessage* data = be_clipboard->Data();
    status_t result = data
        ? data->AddData("text/plain", B_MIME_TYPE, text.c_str(), text.size() + 1)
        : B_ERROR;
    if (result == B_OK) result = be_clipboard->Commit();
    be_clipboard->Unlock();
    return result == B_OK;
}

bool DefaultBrowserIsWebPositive() {
//---------------------------------------------------------------------------------------------------------------------------------//

    entry_ref reference;
    if (!be_roster || be_roster->FindApp("text/html", &reference) != B_OK) return false;
    BEntry entry(&reference, true);
    BPath path;
    if (entry.GetPath(&path) != B_OK || !path.Leaf()) return false;
    std::string name = path.Leaf();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return name.find("webpositive") != std::string::npos;
}

std::string CallbackPortForLog(const std::string& authorization_url) {
//---------------------------------------------------------------------------------------------------------------------------------//

    const char* markers[] = {"127.0.0.1%3A", "127.0.0.1%3a", "127.0.0.1:"};
    for (const char* marker : markers) {
        std::size_t position = authorization_url.find(marker);
        if (position == std::string::npos) continue;
        position += std::strlen(marker);
        std::size_t end = position;
        while (end < authorization_url.size() &&
               std::isdigit(static_cast<unsigned char>(authorization_url[end]))) ++end;
        if (end > position) return authorization_url.substr(position, end - position);
    }
    return "unknown";
}

struct ProviderSettings {
    std::vector<kairo::ProviderProfile> profiles;
    std::string selected_provider_id;
};

enum class TranscriptStyle {
    Assistant,
    User,
    Tool,
    ToolOutput,
    Warning,
    Error,
};

fs::path ProviderSettingsPath() {
    return SessionDirectory().parent_path() / "provider_settings";
}

ProviderSettings LoadProviderSettings() {
    ProviderSettings settings{kairo::DefaultProviderProfiles(), "openai"};

    // Preserve the original environment-only configuration as the custom profile.
    auto environment_custom = std::find_if(settings.profiles.begin(), settings.profiles.end(),
        [](const kairo::ProviderProfile& profile) { return profile.id == "custom"; });
    const char* legacy_endpoint = std::getenv("KAIRO_BASE_URL");
    const char* legacy_key = std::getenv("KAIRO_API_KEY");
    if (environment_custom != settings.profiles.end()
        && ((legacy_endpoint && *legacy_endpoint) || (legacy_key && *legacy_key))) {
        if (legacy_endpoint && *legacy_endpoint) environment_custom->base_url = legacy_endpoint;
        settings.selected_provider_id = environment_custom->id;
    }

    BFile file(ProviderSettingsPath().c_str(), B_READ_ONLY);
    BMessage archive;
    if (file.InitCheck() != B_OK || archive.Unflatten(&file) != B_OK) return settings;
    int32 version = 1;
    archive.FindInt32("version", &version);
    const char* value = nullptr;
    if (version >= 2) {
        ProviderSettings loaded;
        if (archive.FindString("selected_provider", &value) == B_OK && value)
            loaded.selected_provider_id = value;
        for (int32 index = 0;; ++index) {
            BMessage item;
            if (archive.FindMessage("profile", index, &item) != B_OK) break;
            kairo::ProviderProfile profile;
            if (item.FindString("id", &value) == B_OK && value) profile.id = value;
            if (item.FindString("name", &value) == B_OK && value) profile.name = value;
            if (item.FindString("kind", &value) == B_OK && value) {
                try { profile.kind = kairo::ParseProviderKind(value); }
                catch (...) { continue; }
            }
            if (item.FindString("endpoint", &value) == B_OK && value) profile.base_url = value;
            if (item.FindString("key_environment", &value) == B_OK && value)
                profile.api_key_environment = value;
            if (item.FindString("api_key", &value) == B_OK && value) profile.api_key = value;
            if (item.FindString("selected_model", &value) == B_OK && value)
                profile.selected_model = value;
            for (int32 model_index = 0;
                 item.FindString("model", model_index, &value) == B_OK; ++model_index)
                if (value && *value) profile.models.emplace_back(value);
            if (!profile.id.empty() && !profile.name.empty() && !profile.base_url.empty()
                && !profile.models.empty())
                loaded.profiles.push_back(std::move(profile));
        }
        if (!loaded.profiles.empty()) {
            for (const auto& default_profile : kairo::DefaultProviderProfiles()) {
                const bool present = std::any_of(loaded.profiles.begin(), loaded.profiles.end(),
                    [&](const kairo::ProviderProfile& profile) {
                        return profile.id == default_profile.id;
                    });
                if (!present) loaded.profiles.push_back(default_profile);
            }
            if (loaded.selected_provider_id.empty())
                loaded.selected_provider_id = loaded.profiles.front().id;
            return loaded;
        }
    }

    // Migrate the original single-provider settings into the custom profile.
    auto custom = std::find_if(settings.profiles.begin(), settings.profiles.end(),
        [](const kairo::ProviderProfile& profile) { return profile.id == "custom"; });
    if (custom != settings.profiles.end()) {
        if (archive.FindString("endpoint", &value) == B_OK && value && *value)
            custom->base_url = value;
        if (archive.FindString("api_key", &value) == B_OK && value)
            custom->api_key = value;
        settings.selected_provider_id = custom->id;
    }
    return settings;
}

void AddSettingsToMessage(BMessage& archive, const ProviderSettings& settings) {
    archive.AddInt32("version", 2);
    archive.AddString("selected_provider", settings.selected_provider_id.c_str());
    for (const auto& profile : settings.profiles) {
        BMessage item('kprf');
        item.AddString("id", profile.id.c_str());
        item.AddString("name", profile.name.c_str());
        item.AddString("kind", kairo::ProviderKindId(profile.kind).c_str());
        item.AddString("endpoint", profile.base_url.c_str());
        item.AddString("key_environment", profile.api_key_environment.c_str());
        item.AddString("api_key", profile.api_key.c_str());
        item.AddString("selected_model", profile.selected_model.c_str());
        for (const auto& model : profile.models) item.AddString("model", model.c_str());
        archive.AddMessage("profile", &item);
    }
}

ProviderSettings SettingsFromMessage(const BMessage& archive) {
    ProviderSettings settings;
    const char* value = nullptr;
    if (archive.FindString("selected_provider", &value) == B_OK && value)
        settings.selected_provider_id = value;
    for (int32 index = 0;; ++index) {
        BMessage item;
        if (archive.FindMessage("profile", index, &item) != B_OK) break;
        kairo::ProviderProfile profile;
        if (item.FindString("id", &value) == B_OK && value) profile.id = value;
        if (item.FindString("name", &value) == B_OK && value) profile.name = value;
        if (item.FindString("kind", &value) == B_OK && value)
            profile.kind = kairo::ParseProviderKind(value);
        if (item.FindString("endpoint", &value) == B_OK && value) profile.base_url = value;
        if (item.FindString("key_environment", &value) == B_OK && value)
            profile.api_key_environment = value;
        if (item.FindString("api_key", &value) == B_OK && value) profile.api_key = value;
        if (item.FindString("selected_model", &value) == B_OK && value)
            profile.selected_model = value;
        for (int32 model_index = 0;
             item.FindString("model", model_index, &value) == B_OK; ++model_index)
            if (value && *value) profile.models.emplace_back(value);
        settings.profiles.push_back(std::move(profile));
    }
    return settings;
}

status_t SaveProviderSettings(const ProviderSettings& settings) {
    fs::path path = ProviderSettingsPath();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) return B_ERROR;
    if (chmod(path.parent_path().c_str(), S_IRWXU) != 0) return B_ERROR;

    std::string pattern = path.string() + ".tmp.XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    int descriptor = mkstemp(writable.data());
    if (descriptor < 0) return B_ERROR;
    fs::path temporary(writable.data());
    close(descriptor);
    if (chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
        fs::remove(temporary, error);
        return B_ERROR;
    }

    BMessage archive('kpst');
    AddSettingsToMessage(archive, settings);
    status_t result;
    {
        BFile file(temporary.c_str(), B_WRITE_ONLY | B_ERASE_FILE);
        result = file.InitCheck();
        if (result == B_OK) result = archive.Flatten(&file);
    }
    if (result != B_OK) {
        fs::remove(temporary, error);
        return result;
    }
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return B_ERROR;
    }
    return B_OK;
}

class ProviderSettingsWindow : public BWindow {
public:
    ProviderSettingsWindow(BMessenger target, const ProviderSettings& settings)
        : BWindow(BRect(180, 180, 870, 650), "Kairo Provider Settings",
                  B_TITLED_WINDOW, B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS),
          target_(target), profiles_(settings.profiles),
          selected_provider_id_(settings.selected_provider_id) {
        if (profiles_.empty()) profiles_ = kairo::DefaultProviderProfiles();
        provider_menu_ = new BPopUpMenu("settings-provider-options", true, true);
        provider_ = new BMenuField("settings-provider", "Provider", provider_menu_);
        kind_menu_ = new BPopUpMenu("settings-kind-options", true, true);
        for (int32 index = 0; index < kairo::ProviderKindCount(); ++index) {
            auto kind = static_cast<kairo::ProviderKind>(index);
            BMessage* selected = new BMessage(kSettingsKindChanged);
            selected->AddInt32("index", index);
            kind_menu_->AddItem(new BMenuItem(kairo::ProviderKindName(kind), selected));
        }
        kind_ = new BMenuField("settings-kind", "Type", kind_menu_);
        name_ = new BTextControl("settings-name", "Name", "", nullptr);
        endpoint_ = new BTextControl("settings-endpoint", "API URL", "", nullptr);
        key_environment_ = new BTextControl("settings-key-environment", "Key variable", "", nullptr);
        api_key_ = new BTextControl("settings-api-key", "API key", "", nullptr);
        api_key_->TextView()->HideTyping(true);
        models_ = new BTextView("settings-models");
        models_scroll_ = new BScrollView("settings-models-scroll", models_, 0, false, true);
        chatgpt_sign_in_ = new BButton("chatgpt-sign-in", "Continue with ChatGPT",
                                       new BMessage(kChatGPTSignIn));
        chatgpt_sign_out_ = new BButton("chatgpt-sign-out", "Sign Out",
                                        new BMessage(kChatGPTSignOut));
        chatgpt_status_ = new BStringView("chatgpt-status", "");
        provider_->SetExplicitMinSize(BSize(560, B_SIZE_UNSET));
        name_->SetExplicitMinSize(BSize(560, B_SIZE_UNSET));
        endpoint_->SetExplicitMinSize(BSize(560, B_SIZE_UNSET));
        key_environment_->SetExplicitMinSize(BSize(560, B_SIZE_UNSET));
        api_key_->SetExplicitMinSize(BSize(560, B_SIZE_UNSET));
        models_scroll_->SetExplicitMinSize(BSize(560, 130));
        auto* add = new BButton("settings-add", "Add Provider",
                                new BMessage(kSettingsAddProvider));
        auto* remove = new BButton("settings-delete", "Delete Provider",
                                   new BMessage(kSettingsDeleteProvider));
        auto* cancel = new BButton("settings-cancel", "Cancel",
                                   new BMessage(B_QUIT_REQUESTED));
        auto* save = new BButton("settings-save", "Save All",
                                 new BMessage(kSaveSettings));
        SetDefaultButton(save);

        BLayoutBuilder::Group<>(this, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_WINDOW_INSETS)
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(provider_, 1.0f)
                .Add(add)
                .Add(remove)
            .End()
            .Add(name_)
            .Add(kind_)
            .Add(endpoint_)
            .Add(key_environment_)
            .Add(api_key_)
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(chatgpt_sign_in_)
                .Add(chatgpt_sign_out_)
                .Add(chatgpt_status_, 1.0f)
            .End()
            .Add(new BStringView("models-label", "Models (one API model ID per line)"))
            .Add(models_scroll_, 1.0f)
            .Add(credential_note_ = new BStringView("credential-note", ""))
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .AddGlue()
                .Add(cancel)
                .Add(save)
            .End();
        RebuildProviderMenu();
        current_ = 0;
        for (std::size_t index = 0; index < profiles_.size(); ++index)
            if (profiles_[index].id == selected_provider_id_) current_ = index;
        LoadCurrent();
    }

    ~ProviderSettingsWindow() override { auth_cancellation_.Cancel(); }

    bool QuitRequested() override {
        if (auth_busy_) {
            BAlert* alert = new BAlert("cancel-chatgpt-sign-in",
                "ChatGPT sign-in is still waiting for the browser callback. Keep this settings window open until Kairo reports that it is connected.",
                "Keep Waiting", "Cancel Sign-in", nullptr,
                B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            if (alert->Go() != 1) return false;
        }
        auth_cancellation_.Cancel();
        return true;
    }

    void MessageReceived(BMessage* message) override {
        if (message->what == kActivateSettings) {
            Activate(true);
            return;
        }
        if (message->what == kSettingsProviderChanged) {
            int32 index = 0;
            if (message->FindInt32("index", &index) == B_OK && index >= 0
                && static_cast<std::size_t>(index) < profiles_.size()) {
                StoreCurrent();
                current_ = static_cast<std::size_t>(index);
                LoadCurrent();
            }
            return;
        }
        if (message->what == kSettingsKindChanged) {
            int32 index = 0;
            if (message->FindInt32("index", &index) == B_OK && index >= 0
                && index < kairo::ProviderKindCount()) {
                profiles_[current_].kind = static_cast<kairo::ProviderKind>(index);
                if (kairo::ProviderUsesChatGPTPlan(profiles_[current_].kind)) {
                    profiles_[current_].base_url = "https://api.openai.com/v1";
                    profiles_[current_].api_key.clear();
                    profiles_[current_].api_key_environment.clear();
                    endpoint_->SetText(profiles_[current_].base_url.c_str());
                    key_environment_->SetText("");
                    api_key_->SetText("");
                }
                UpdateKindControls();
            }
            return;
        }
        if (message->what == kSettingsAddProvider) { AddProvider(); return; }
        if (message->what == kSettingsDeleteProvider) { DeleteProvider(); return; }
        if (message->what == kChatGPTSignIn) { StartChatGPTSignIn(); return; }
        if (message->what == kChatGPTSignOut) { StartChatGPTSignOut(); return; }
        if (message->what == kOpenAuthUrl) { OpenAuthUrl(message); return; }
        if (message->what == kChatGPTAuthResult) { FinishChatGPTAuth(message); return; }
        if (message->what == kSaveSettings) { SaveAll(); return; }
        BWindow::MessageReceived(message);
    }

private:
    static void ClearMenu(BMenu* menu) {
        while (BMenuItem* item = menu->RemoveItem(static_cast<int32>(0))) delete item;
    }

    static std::vector<std::string> ParseModels(const char* text) {
        std::vector<std::string> models;
        std::string input = text ? text : "";
        std::size_t start = 0;
        while (start <= input.size()) {
            std::size_t end = input.find('\n', start);
            std::string model = input.substr(start, end == std::string::npos
                ? std::string::npos : end - start);
            while (!model.empty() && (model.back() == '\r' || model.back() == ' '
                   || model.back() == '\t')) model.pop_back();
            std::size_t first = model.find_first_not_of(" \t");
            if (first != std::string::npos) models.push_back(model.substr(first));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return models;
    }

    bool StoreCurrent() {
        if (current_ >= profiles_.size()) return false;
        auto& profile = profiles_[current_];
        profile.name = name_->Text();
        profile.base_url = endpoint_->Text();
        profile.api_key_environment = key_environment_->Text();
        profile.api_key = api_key_->Text();
        profile.models = ParseModels(models_->Text());
        BMenuItem* marked_kind = kind_menu_->FindMarked();
        if (marked_kind) profile.kind = static_cast<kairo::ProviderKind>(kind_menu_->IndexOf(marked_kind));
        if (profile.selected_model.empty() ||
            std::find(profile.models.begin(), profile.models.end(), profile.selected_model) == profile.models.end())
            profile.selected_model = profile.models.empty() ? "" : profile.models.front();
        return !profile.name.empty() && !profile.base_url.empty() && !profile.models.empty();
    }

    void LoadCurrent() {
        const auto& profile = profiles_[current_];
        name_->SetText(profile.name.c_str());
        endpoint_->SetText(profile.base_url.c_str());
        key_environment_->SetText(profile.api_key_environment.c_str());
        api_key_->SetText(profile.api_key.c_str());
        std::string model_lines;
        for (const auto& model : profile.models) model_lines += model + "\n";
        models_->SetText(model_lines.c_str());
        if (auto* item = provider_menu_->ItemAt(static_cast<int32>(current_))) item->SetMarked(true);
        if (auto* item = kind_menu_->ItemAt(static_cast<int32>(profile.kind))) item->SetMarked(true);
        UpdateKindControls();
    }

    void UpdateKindControls() {
        const bool chatgpt = kairo::ProviderUsesChatGPTPlan(profiles_[current_].kind);
        endpoint_->SetLabel("API URL");
        endpoint_->SetEnabled(!chatgpt);
        key_environment_->SetEnabled(!chatgpt);
        api_key_->SetEnabled(!chatgpt);
        models_->MakeEditable(!chatgpt);
        bool signed_in = false;
        bool account_known = false;
        if (chatgpt) {
            try {
                kairo::ChatGPTAuth auth(ChatGPTCredentialPath());
                auto account = auth.Account();
                account_known = !account.client_id.empty() && !account.subject.empty() &&
                                !account.id_token.empty();
                signed_in = auth.IsSignedIn();
                if (!auth_busy_) chatgpt_status_->SetText(signed_in
                    ? ("Connected as " + (account.email.empty() ? std::string("ChatGPT account") : account.email)).c_str()
                    : (account_known ? "Connected; plan access is not enabled" : "Not connected"));
            } catch (...) { chatgpt_status_->SetText("Not connected"); }
            credential_note_->SetText(
                "Uses your ChatGPT plan through Kairo's secure OAuth connection; no Codex executable or API key.");
        } else {
            chatgpt_status_->SetText("");
            credential_note_->SetText(
                "API keys are stored locally with owner-only permissions; prefer an environment variable.");
        }
        chatgpt_sign_in_->SetLabel(account_known ? "Reconnect ChatGPT" : "Continue with ChatGPT");
        chatgpt_sign_in_->SetEnabled(chatgpt && !auth_busy_);
        chatgpt_sign_out_->SetEnabled(chatgpt && account_known && !auth_busy_);
    }

    void StartChatGPTSignIn() {
        if (auth_busy_ || !kairo::ProviderUsesChatGPTPlan(profiles_[current_].kind)) return;
        WriteLog("INFO", "ChatGPT sign-in started; waiting for loopback callback");
        auth_busy_ = true;
        auth_cancellation_ = kairo::CancellationToken{};
        UpdateKindControls();
        chatgpt_status_->SetText("Waiting for browser authorization...");
        BMessenger self(this);
        const fs::path credentials = ChatGPTCredentialPath();
        const std::string profile_id = profiles_[current_].id;
        const kairo::CancellationToken cancellation = auth_cancellation_;
        std::thread([self, credentials, profile_id, cancellation] {
            BMessage result(kChatGPTAuthResult);
            result.AddString("profile_id", profile_id.c_str());
            try {
                kairo::ChatGPTAuth auth(credentials);
                auto account = auth.SignIn([self](const std::string& url) {
                    BMessage open(kOpenAuthUrl);
                    open.AddString("url", url.c_str());
                    self.SendMessage(&open);
                }, cancellation);
                result.AddBool("ok", true);
                result.AddString("email", account.email.c_str());
                try {
                    auto models = auth.ListModels();
                    if (models.empty())
                        throw std::runtime_error("OpenAI returned no available ChatGPT models");
                    for (const auto& model : models)
                        result.AddString("model", model.slug.c_str());
                } catch (const std::exception& error) {
                    result.AddString("model_error", error.what());
                }
            } catch (const std::exception& error) {
                result.AddBool("ok", false);
                result.AddString("error", error.what());
            }
            self.SendMessage(&result);
        }).detach();
    }

    void StartChatGPTSignOut() {
        if (auth_busy_ || !kairo::ProviderUsesChatGPTPlan(profiles_[current_].kind)) return;
        WriteLog("INFO", "ChatGPT sign-out started");
        auth_busy_ = true;
        UpdateKindControls();
        chatgpt_status_->SetText("Signing out...");
        BMessenger self(this);
        const fs::path credentials = ChatGPTCredentialPath();
        const std::string profile_id = profiles_[current_].id;
        std::thread([self, credentials, profile_id] {
            BMessage result(kChatGPTAuthResult);
            result.AddString("profile_id", profile_id.c_str());
            result.AddBool("signed_out", true);
            try { kairo::ChatGPTAuth(credentials).SignOut(); result.AddBool("ok", true); }
            catch (const std::exception& error) {
                result.AddBool("ok", false); result.AddString("error", error.what());
            }
            self.SendMessage(&result);
        }).detach();
    }

    void OpenAuthUrl(BMessage* message) {
        const char* url = nullptr;
        if (message->FindString("url", &url) != B_OK || !url) return;
        const bool copied = CopyTextToClipboard(url);
        const bool webpositive = DefaultBrowserIsWebPositive();
        const std::string callback_port = CallbackPortForLog(url);
        status_t opened = B_OK;
        if (!webpositive) {
            const char* arguments[] = {url};
            opened = be_roster->Launch("text/html", 1, arguments);
        }
        WriteLog(opened == B_OK ? "INFO" : "WARNING",
            std::string(webpositive
                ? "WebPositive launch skipped for ChatGPT authorization"
                : "ChatGPT authorization handed to the default browser; result=" + std::to_string(opened))
            + "; callback_port=" + callback_port
            + "; URL copied=" + (copied ? "yes" : "no"));
        if (webpositive) {
            chatgpt_status_->SetText(copied
                ? "Open Firefox and paste the copied sign-in URL"
                : "Set Firefox as the default browser and retry");
            (new BAlert("chatgpt-firefox",
                copied
                    ? "WebPositive cannot reliably complete OpenAI's security check. Kairo did not open it. Open Firefox, paste the sign-in URL from the clipboard, and keep this settings window open."
                    : "WebPositive cannot reliably complete OpenAI's security check. Set Firefox as the default browser, then retry Continue with ChatGPT.",
                "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT))->Go();
            return;
        }
        if (opened != B_OK) {
            std::string text = "Kairo could not open the default browser.";
            text += copied ? " The sign-in URL is on the clipboard; paste it into Firefox."
                           : " Please set Firefox as the default browser and try again.";
            (new BAlert("chatgpt-url", text.c_str(), "OK", nullptr, nullptr,
                        B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go();
            return;
        }
        chatgpt_status_->SetText(copied
            ? "Waiting for browser; sign-in URL copied for Firefox"
            : "Waiting for browser authorization...");
        (new BAlert("chatgpt-browser",
            copied
                ? "Kairo opened your default browser and copied the sign-in URL. If WebPositive shows security or Cloudflare errors, paste the URL into Firefox."
                : "Kairo opened your default browser. If WebPositive shows security or Cloudflare errors, set Firefox as the default browser and retry.",
            "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_INFO_ALERT))->Go();
    }

    void FinishChatGPTAuth(BMessage* message) {
        auth_busy_ = false;
        bool ok = false, signed_out = false;
        message->FindBool("ok", &ok);
        message->FindBool("signed_out", &signed_out);
        if (ok && !signed_out) {
            const char* profile_id = nullptr;
            message->FindString("profile_id", &profile_id);
            auto profile = std::find_if(profiles_.begin(), profiles_.end(),
                [profile_id](const kairo::ProviderProfile& candidate) {
                    return profile_id && candidate.id == profile_id;
                });
            if (profile == profiles_.end()) return;
            std::vector<std::string> models;
            const char* value = nullptr;
            for (int32 index = 0; message->FindString("model", index, &value) == B_OK; ++index)
                if (value && *value) models.emplace_back(value);
            if (!models.empty()) {
                profile->models = std::move(models);
                profile->selected_model = profile->models.front();
                if (&*profile == &profiles_[current_]) {
                    std::string lines;
                    for (const auto& model : profile->models) lines += model + "\n";
                    models_->SetText(lines.c_str());
                }
            }
            const char* model_error = nullptr;
            if (message->FindString("model_error", &model_error) == B_OK && model_error) {
                UpdateKindControls();
                chatgpt_status_->SetText("Connected; model list unavailable");
                WriteLog("WARNING", std::string("ChatGPT connected but model discovery failed: ")
                    + model_error);
                std::string detail = "ChatGPT authorization succeeded, but Kairo could not load the model list:\n\n";
                detail += model_error;
                (new BAlert("chatgpt-models-error", detail.c_str(), "OK", nullptr, nullptr,
                            B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go();
                return;
            }
            chatgpt_status_->SetText("Connected; saving model list...");
            WriteLog("INFO", "ChatGPT sign-in completed and model list was received");
            SaveAll();
            return;
        } else if (ok) {
            chatgpt_status_->SetText("Signed out");
            WriteLog("INFO", "ChatGPT sign-out completed");
        }
        else {
            const char* error = "ChatGPT authorization failed";
            message->FindString("error", &error);
            chatgpt_status_->SetText("Authorization failed");
            WriteLog("ERROR", std::string("ChatGPT account operation failed: ") + error);
            (new BAlert("chatgpt-error", error, "OK", nullptr, nullptr,
                        B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
        }
        UpdateKindControls();
    }

    void RebuildProviderMenu() {
        ClearMenu(provider_menu_);
        for (std::size_t index = 0; index < profiles_.size(); ++index) {
            BMessage* selected = new BMessage(kSettingsProviderChanged);
            selected->AddInt32("index", static_cast<int32>(index));
            provider_menu_->AddItem(new BMenuItem(profiles_[index].name.c_str(), selected));
        }
    }

    void AddProvider() {
        StoreCurrent();
        const std::string id_base = "provider-" + std::to_string(std::time(nullptr));
        std::string id = id_base;
        for (std::size_t suffix = 2; std::any_of(profiles_.begin(), profiles_.end(),
                 [&id](const kairo::ProviderProfile& profile) { return profile.id == id; }); ++suffix)
            id = id_base + "-" + std::to_string(suffix);
        kairo::ProviderProfile profile{
            std::move(id),
            "New Provider", kairo::ProviderKind::OpenAICompatible,
            "https://openrouter.ai/api/v1", "KAIRO_API_KEY", {}, {"model-id"}, "model-id"};
        profiles_.push_back(std::move(profile));
        current_ = profiles_.size() - 1;
        RebuildProviderMenu();
        LoadCurrent();
        name_->MakeFocus(true);
    }

    void DeleteProvider() {
        if (profiles_.size() <= 1) return;
        profiles_.erase(profiles_.begin() + static_cast<std::ptrdiff_t>(current_));
        if (current_ >= profiles_.size()) current_ = profiles_.size() - 1;
        RebuildProviderMenu();
        LoadCurrent();
    }

    void SaveAll() {
        if (auth_busy_) {
            (new BAlert("chatgpt-sign-in-active",
                "Wait for ChatGPT sign-in to finish before saving and closing this window.",
                "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go();
            return;
        }
        if (!StoreCurrent()) {
            (new BAlert("provider-invalid",
                "Each provider needs a name, an API URL, and at least one model ID.",
                "OK", nullptr, nullptr, B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
            return;
        }
        for (const auto& profile : profiles_) {
            try {
                kairo::ValidateProviderProfile(profile);
                (void)kairo::CreateProvider(profile, 120, ChatGPTCredentialPath().string());
            } catch (const std::exception& error) {
                (new BAlert("provider-invalid", error.what(), "OK", nullptr, nullptr,
                            B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
                return;
            }
        }
        ProviderSettings settings{profiles_, profiles_[current_].id};
        BMessage saved(kSaveSettings);
        AddSettingsToMessage(saved, settings);
        target_.SendMessage(&saved);
        PostMessage(B_QUIT_REQUESTED);
    }

    BMessenger target_;
    std::vector<kairo::ProviderProfile> profiles_;
    std::string selected_provider_id_;
    std::size_t current_ = 0;
    BMenuField* provider_;
    BPopUpMenu* provider_menu_;
    BMenuField* kind_;
    BPopUpMenu* kind_menu_;
    BTextControl* name_;
    BTextControl* endpoint_;
    BTextControl* key_environment_;
    BTextControl* api_key_;
    BTextView* models_;
    BButton* chatgpt_sign_in_;
    BButton* chatgpt_sign_out_;
    BStringView* chatgpt_status_;
    bool auth_busy_ = false;
    kairo::CancellationToken auth_cancellation_;
    BScrollView* models_scroll_;
    BStringView* credential_note_;
};

class KairoWindow : public BWindow {
public:
    KairoWindow()
        : BWindow(BRect(80, 80, 1180, 830), "Kairo", B_TITLED_WINDOW,
                  B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS) {
        menu_bar_ = new BMenuBar("menu-bar");
        auto* project_menu = new BMenu("Project");
        choose_project_item_ = new BMenuItem("Choose Project...", new BMessage(kChooseProject), 'O');
        project_menu->AddItem(choose_project_item_);
        project_menu->AddSeparatorItem();
        project_menu->AddItem(new BMenuItem("Quit Kairo", new BMessage(kQuit), 'Q'));
        menu_bar_->AddItem(project_menu);

        auto* session_menu = new BMenu("Session");
        new_session_item_ = new BMenuItem("New Session", new BMessage(kNewSession), 'N');
        session_menu->AddItem(new_session_item_);
        session_menu->AddItem(new BMenuItem("Clear Conversation", new BMessage(kClearTranscript), 'L'));
        menu_bar_->AddItem(session_menu);

        auto* run_menu = new BMenu("Run");
        focus_prompt_item_ = new BMenuItem("Focus Prompt", new BMessage(kFocusPrompt), 'P');
        send_item_ = new BMenuItem("Send Prompt", new BMessage(kSend), 'R');
        cancel_item_ = new BMenuItem("Cancel Run", new BMessage(kCancel), 'K');
        cancel_item_->SetEnabled(false);
        run_menu->AddItem(focus_prompt_item_);
        run_menu->AddSeparatorItem();
        run_menu->AddItem(send_item_);
        run_menu->AddItem(cancel_item_);
        menu_bar_->AddItem(run_menu);

        auto* settings_menu = new BMenu("Settings");
        settings_item_ = new BMenuItem("Provider Settings...",
                                       new BMessage(kOpenSettings), ',');
        settings_menu->AddItem(settings_item_);
        menu_bar_->AddItem(settings_menu);

        auto* help_menu = new BMenu("Help");
        help_menu->AddItem(new BMenuItem("About Kairo...", new BMessage(B_ABOUT_REQUESTED)));
        menu_bar_->AddItem(help_menu);

        project_ = new BTextControl("project", "Project", "/boot/home/work", nullptr);
        choose_ = new BButton("choose", "Choose", new BMessage(kChooseProject));
        session_ = new BTextControl("session", "Resume session", "", nullptr);
        provider_settings_ = LoadProviderSettings();
        settings_ = new BButton("settings", "Settings...", new BMessage(kOpenSettings));
        provider_menu_ = new BPopUpMenu("provider-options", true, true);
        provider_ = new BMenuField("provider", "Provider", provider_menu_);
        model_menu_ = new BPopUpMenu("model-options", true, true);
        model_ = new BMenuField("model", "Model", model_menu_);
        RebuildProviderMenu();
        RebuildModelMenu();
        approval_checkbox_ = new BCheckBox("approvals",
            "Ask before file writes and shell commands",
            new BMessage(kApprovalModeChanged));
        approval_checkbox_->SetValue(B_CONTROL_ON);
        transcript_ = new BTextView("transcript");
        transcript_->SetStylable(true);
        transcript_->MakeEditable(false);
        prompt_ = new BTextView("prompt");
        status_ = new BStringView("status", ReadyStatus(ActiveProfile()).c_str());
        send_ = new BButton("send", "Send", new BMessage(kSend));
        cancel_ = new BButton("cancel", "Cancel", new BMessage(kCancel));
        cancel_->SetEnabled(false);

        auto* transcript_scroll = new BScrollView("transcript-scroll", transcript_, 0, false, true);
        auto* prompt_scroll = new BScrollView("prompt-scroll", prompt_, 0, false, true);
        project_->SetExplicitMinSize(BSize(650, B_SIZE_UNSET));
        session_->SetExplicitMinSize(BSize(350, B_SIZE_UNSET));
        provider_->SetExplicitMinSize(BSize(210, B_SIZE_UNSET));
        model_->SetExplicitMinSize(BSize(280, B_SIZE_UNSET));
        transcript_scroll->SetExplicitMinSize(BSize(800, 320));
        prompt_scroll->SetExplicitMinSize(BSize(800, 140));
        auto* setup_box = new BBox("setup-box");
        setup_box->SetLabel("Project and session");
        BLayoutBuilder::Group<>(setup_box, B_VERTICAL, B_USE_DEFAULT_SPACING)
            .SetInsets(B_USE_DEFAULT_SPACING)
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(project_, 1.0f)
                .Add(choose_)
            .End()
                .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                    .Add(session_, 1.0f)
                    .Add(provider_)
                    .Add(model_)
                    .Add(settings_)
            .End()
            .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                .Add(approval_checkbox_)
                .AddGlue()
            .End();

        BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
            .SetInsets(0)
            .Add(menu_bar_)
            .AddGroup(B_VERTICAL, B_USE_DEFAULT_SPACING)
                .SetInsets(B_USE_WINDOW_INSETS)
                .Add(setup_box)
                .Add(new BStringView("conversation-label", "Conversation"))
                .Add(transcript_scroll, 1.0f)
                .Add(new BStringView("prompt-label", "Prompt"))
                .Add(prompt_scroll, 0.28f)
                .AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
                    .Add(status_, 1.0f)
                    .Add(cancel_)
                    .Add(send_)
                .End()
            .End();
        WriteLog("INFO", std::string("Kairo GUI ") + ApplicationVersion() + " started");
    }

    ~KairoWindow() override {
        WriteLog("INFO", "Kairo GUI stopping");
        cancellation_.Cancel();
        ResolveAllApprovals(false);
        if (worker_.joinable()) worker_.join();
        delete project_panel_;
    }

    bool QuitRequested() override {
        cancellation_.Cancel();
        ResolveAllApprovals(false);
        be_app->PostMessage(B_QUIT_REQUESTED);
        return true;
    }

    void MessageReceived(BMessage* message) override {
        switch (message->what) {
            case kSend: StartRun(); break;
            case kCancel:
                WriteLog("INFO", "Run cancellation requested");
                cancellation_.Cancel();
                ResolveAllApprovals(false);
                status_->SetText("Cancelling");
                break;
            case kChooseProject: ChooseProject(); break;
            case kNewSession: NewSession(); break;
            case kClearTranscript: transcript_->SetText(""); break;
            case kFocusPrompt: prompt_->MakeFocus(true); break;
            case kQuit: be_app->PostMessage(B_QUIT_REQUESTED); break;
            case kOpenSettings: OpenSettings(); break;
            case kSaveSettings: ReceiveSettings(message); break;
            case kProviderChanged: ChangeProvider(message); break;
            case kModelChanged: ChangeModel(message); break;
            case kApprovalModeChanged: ConfirmApprovalMode(); break;
            case B_ABOUT_REQUESTED: ShowAbout(); break;
            case B_REFS_RECEIVED: ReceiveProject(message); break;
            case kEngineEvent: ReceiveEngineEvent(message); break;
            case kApprovalRequest: ShowApproval(message); break;
            case kApprovalDecision: FinishApproval(message); break;
            default: BWindow::MessageReceived(message);
        }
    }

private:
    static void ClearMenu(BMenu* menu) {
        while (BMenuItem* item = menu->RemoveItem(static_cast<int32>(0))) delete item;
    }

    kairo::ProviderProfile& ActiveProfile() {
        return provider_settings_.profiles[selected_provider_index_];
    }

    const kairo::ProviderProfile& ActiveProfile() const {
        return provider_settings_.profiles[selected_provider_index_];
    }

    void RebuildProviderMenu() {
        if (provider_settings_.profiles.empty())
            provider_settings_.profiles = kairo::DefaultProviderProfiles();
        selected_provider_index_ = 0;
        for (std::size_t index = 0; index < provider_settings_.profiles.size(); ++index)
            if (provider_settings_.profiles[index].id == provider_settings_.selected_provider_id)
                selected_provider_index_ = index;
        provider_settings_.selected_provider_id = ActiveProfile().id;
        ClearMenu(provider_menu_);
        for (std::size_t index = 0; index < provider_settings_.profiles.size(); ++index) {
            BMessage* selected = new BMessage(kProviderChanged);
            selected->AddInt32("index", static_cast<int32>(index));
            provider_menu_->AddItem(new BMenuItem(
                provider_settings_.profiles[index].name.c_str(), selected));
        }
        provider_menu_->ItemAt(static_cast<int32>(selected_provider_index_))->SetMarked(true);
    }

    void RebuildModelMenu() {
        ClearMenu(model_menu_);
        auto& profile = ActiveProfile();
        if (profile.models.empty()) profile.models.push_back("model-id");
        std::size_t selected = 0;
        for (std::size_t index = 0; index < profile.models.size(); ++index) {
            BMessage* changed = new BMessage(kModelChanged);
            changed->AddInt32("index", static_cast<int32>(index));
            model_menu_->AddItem(new BMenuItem(profile.models[index].c_str(), changed));
            if (profile.models[index] == profile.selected_model) selected = index;
        }
        profile.selected_model = profile.models[selected];
        model_menu_->ItemAt(static_cast<int32>(selected))->SetMarked(true);
    }

    void ChangeProvider(BMessage* message) {
        if (worker_.joinable()) return;
        int32 index = 0;
        if (message->FindInt32("index", &index) != B_OK || index < 0
            || static_cast<std::size_t>(index) >= provider_settings_.profiles.size()) return;
        selected_provider_index_ = static_cast<std::size_t>(index);
        provider_settings_.selected_provider_id = ActiveProfile().id;
        RebuildModelMenu();
        WriteLog("INFO", std::string("Active provider changed; id=") + ActiveProfile().id
            + "; kind=" + kairo::ProviderKindId(ActiveProfile().kind));
        status_->SetText(ReadyStatus(ActiveProfile()).c_str());
        SaveProviderSettings(provider_settings_);
    }

    void ChangeModel(BMessage* message) {
        if (worker_.joinable()) return;
        int32 index = 0;
        auto& profile = ActiveProfile();
        if (message->FindInt32("index", &index) != B_OK || index < 0
            || static_cast<std::size_t>(index) >= profile.models.size()) return;
        profile.selected_model = profile.models[static_cast<std::size_t>(index)];
        WriteLog("INFO", std::string("Active model changed; provider=") + profile.id
            + "; model=" + profile.selected_model);
        SaveProviderSettings(provider_settings_);
    }

    void AppendTranscript(const std::string& text, TranscriptStyle style,
                          bool bold = false) {
        if (text.empty()) return;
        rgb_color color{35, 35, 35, 255};
        BFont font;
        font = *be_plain_font;
        switch (style) {
            case TranscriptStyle::Assistant:
                color = {35, 35, 35, 255};
                break;
            case TranscriptStyle::User:
                color = {25, 90, 175, 255};
                break;
            case TranscriptStyle::Tool:
                color = {115, 65, 155, 255};
                break;
            case TranscriptStyle::ToolOutput:
                color = {55, 90, 105, 255};
                font = *be_fixed_font;
                break;
            case TranscriptStyle::Warning:
                color = {185, 105, 0, 255};
                break;
            case TranscriptStyle::Error:
                color = {190, 35, 45, 255};
                break;
        }
        if (bold) font.SetFace(B_BOLD_FACE);
        int32 start = transcript_->TextLength();
        transcript_->Insert(start, text.c_str(), text.size());
        rgb_color applied_color = color;
        transcript_->SetFontAndColor(start, start + static_cast<int32>(text.size()),
                                     &font, B_FONT_ALL, &applied_color);
        transcript_->ScrollToOffset(transcript_->TextLength());
    }

    void NewSession() {
        if (worker_.joinable()) return;
        session_->SetText("");
        transcript_->SetText("");
        status_->SetText(ReadyStatus(ActiveProfile()).c_str());
        prompt_->MakeFocus(true);
        WriteLog("INFO", "New session selected");
    }

    void OpenSettings() {
        if (settings_messenger_.IsValid()) {
            if (settings_messenger_.SendMessage(kActivateSettings) == B_OK) return;
            settings_messenger_ = BMessenger();
        }
        WriteLog("INFO", "Provider settings opened");
        auto* settings_window = new ProviderSettingsWindow(BMessenger(this), provider_settings_);
        settings_messenger_ = BMessenger(settings_window);
        settings_window->MoveTo(Frame().left + 100, Frame().top + 80);
        settings_window->Show();
    }

    void ReceiveSettings(BMessage* message) {
        try {
            ProviderSettings updated = SettingsFromMessage(*message);
            if (updated.profiles.empty()) throw std::runtime_error("No providers were supplied");
            for (const auto& profile : updated.profiles)
                (void)kairo::CreateProvider(profile, 120, ChatGPTCredentialPath().string());
            if (SaveProviderSettings(updated) != B_OK)
                throw std::runtime_error("Kairo could not save the provider settings");
            provider_settings_ = std::move(updated);
            RebuildProviderMenu();
            RebuildModelMenu();
        } catch (const std::exception& error) {
            WriteLog("WARNING", "Invalid provider settings rejected");
            (new BAlert("settings-error", error.what(), "OK", nullptr, nullptr,
                        B_WIDTH_AS_USUAL, B_STOP_ALERT))->Go();
            return;
        }
        WriteLog("INFO", "Provider settings saved");
        status_->SetText(ReadyStatus(ActiveProfile()).c_str());
    }

    void ConfirmApprovalMode() {
        if (approval_checkbox_->Value() == B_CONTROL_ON) {
            WriteLog("INFO", "Per-action approvals enabled");
            return;
        }
        BAlert* warning = new BAlert("unsafe-approvals",
            "Disabling approvals lets model-generated file writes and shell commands run "
            "without review. Those commands have your user account's filesystem access.",
            "Keep enabled", "Disable approvals", nullptr,
            B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        warning->SetShortcut(0, B_ESCAPE);
        if (warning->Go() != 1) {
            approval_checkbox_->SetValue(B_CONTROL_ON);
            WriteLog("INFO", "Per-action approvals kept enabled");
            return;
        }
        WriteLog("WARNING", "Per-action approvals disabled for the next run");
    }

    void ShowAbout() {
//---------------------------------------------------------------------------------------------------------------------------------//

        const std::string version = ApplicationVersion();
        const std::string text =
            "Kairo " + version + "\n\n"
            "Code boldly. Stay native.\n"
            "AI pair programming for Haiku—plan, build, debug, and iterate "
            "without leaving your desktop.\n\n"
            "Designed by Sikosis\n"
            "https://github.com/sikosis/kairo\n\n"
            "Copyright \xC2\xA9 2026 Kairo contributors. MIT License.\n\n"
            "Powered by Haiku's native kits, libcurl, OpenSSL 3, and OpenAI services.\n"
            "libcurl \xC2\xA9 Daniel Stenberg and contributors; curl license.\n"
            "OpenSSL \xC2\xA9 OpenSSL Project Authors; Apache License 2.0.\n"
            "HVIF conversion uses hvif-tools \xC2\xA9 Gerasim Troeglazov; MIT License.\n\n"
            "Haiku\xC2\xAE and the HAIKU logo\xC2\xAE are registered trademarks of Haiku, Inc.\n"
            "Haiku is developed by the Haiku Project.\n"
            "OpenAI, ChatGPT, and GPT are trademarks of OpenAI.\n"
            "Kairo is independent and is not affiliated with or endorsed by OpenAI.";
        BAlert* alert = new BAlert("about-kairo",
            text.c_str(), "OK", nullptr, nullptr,
            B_WIDTH_AS_USUAL, B_INFO_ALERT);
        if (BBitmap* icon = ApplicationIcon()) alert->SetIcon(icon);
        alert->Go();
    }

    void ChooseProject() {
        if (!project_panel_) {
            BMessenger* target = new BMessenger(this);
            project_panel_ = new BFilePanel(B_OPEN_PANEL, target, nullptr, B_DIRECTORY_NODE, false);
            project_panel_->SetButtonLabel(B_DEFAULT_BUTTON, "Choose");
        }
        project_panel_->Show();
    }

    void ReceiveProject(BMessage* message) {
        entry_ref reference;
        if (message->FindRef("refs", &reference) != B_OK) return;
        BEntry entry(&reference, true);
        BPath path;
        if (entry.GetPath(&path) == B_OK) {
            project_->SetText(path.Path());
            WriteLog("INFO", std::string("Project selected; path=") + path.Path());
        }
    }

    void StartRun() {
        if (worker_.joinable()) return;
        std::string project = project_->Text();
        std::string prompt(prompt_->Text(), prompt_->TextLength());
        if (project.empty() || prompt.empty()) { status_->SetText("Choose a project and enter a prompt"); return; }
        kairo::ProviderProfile profile = ActiveProfile();
        std::string api_key = profile.api_key;
        const char* environment_key = profile.api_key_environment.empty()
            ? nullptr : std::getenv(profile.api_key_environment.c_str());
        if (kairo::ProviderUsesApiKey(profile.kind)
            && api_key.empty() && (!environment_key || !*environment_key)) {
            WriteLog("WARNING", "Run blocked because no API key is configured");
            status_->SetText("Configure an API key in Settings");
            OpenSettings();
            return;
        }
        if (kairo::ProviderUsesChatGPTPlan(profile.kind)) {
            try {
                if (!kairo::ChatGPTAuth(ChatGPTCredentialPath()).IsSignedIn()) {
                    status_->SetText("Continue with ChatGPT in Settings first");
                    OpenSettings();
                    return;
                }
            } catch (const std::exception&) {
                status_->SetText("Continue with ChatGPT in Settings first");
                OpenSettings();
                return;
            }
        }
        prompt_->SetText("");
        AppendTranscript("\nYou: ", TranscriptStyle::User, true);
        AppendTranscript(prompt, TranscriptStyle::User);
        AppendTranscript("\n\nKairo: ", TranscriptStyle::Assistant, true);
        SetRunning(true);
        cancellation_ = kairo::CancellationToken{};
        BMessenger target(this);
        std::string session_id = session_->Text();
        BMenuItem* selected_model = model_menu_->FindMarked();
        std::string model = selected_model ? selected_model->Label() : profile.selected_model;
        profile.selected_model = model;
        bool require_approvals = approval_checkbox_->Value() == B_CONTROL_ON;
        WriteLog("INFO", std::string("Run started; provider=") + profile.id
            + "; kind=" + kairo::ProviderKindId(profile.kind) + "; model=" + model
            + "; project=" + project + "; session="
            + (session_id.empty() ? "new" : session_id) + "; approvals="
            + (require_approvals ? "on" : "off"));
        worker_ = std::thread([this, target, project, prompt, session_id, model,
                               profile, api_key, require_approvals]() mutable {
            bool engine_started = false;
            try {
                kairo::SessionStore store(SessionDirectory());
                kairo::Session session = session_id.empty()
                    ? store.Create(project, model, profile.id) : store.Load(session_id);
                if (fs::canonical(session.project) != fs::canonical(project))
                    throw std::runtime_error("The resumed session belongs to another project");
                if (!session.provider.empty() && session.provider != profile.id) {
                    WriteLog("INFO", std::string("Resumed session provider changed from ")
                        + session.provider + " to " + profile.id);
                    for (auto& message : session.messages) message.provider_items.clear();
                }
                session.provider = profile.id;
                if (session.model != model) {
                    WriteLog("INFO", std::string("Resumed session model changed from ")
                        + session.model + " to " + model);
                    session.model = model;
                    for (auto& message : session.messages) message.provider_items.clear();
                }
                auto approval = [this, target, require_approvals](const kairo::ProposedAction& action) {
                    if (!require_approvals) return true;
                    auto pending = std::make_shared<PendingApproval>();
                    {
                        std::lock_guard<std::mutex> lock(approvals_mutex_);
                        approvals_.push_back(pending);
                    }
                    BMessage request(kApprovalRequest);
                    request.AddPointer("approval", pending.get());
                    request.AddString("preview", action.preview.c_str());
                    if (target.SendMessage(&request) != B_OK) { pending->Resolve(false); return false; }
                    return pending->Wait(cancellation_);
                };
                auto events = [target](const kairo::EngineEvent& event) {
                    BMessage update(kEngineEvent);
                    update.AddInt32("type", static_cast<int32>(event.type));
                    update.AddString("text", event.text.c_str());
                    update.AddString("session", event.session_id.c_str());
                    target.SendMessage(&update);
                };
                if (session.messages.empty()) session.messages.push_back({kairo::Role::System,
                    "You are Kairo, a local coding assistant running natively on Haiku. Reads and searches "
                    "stay inside the selected project. Use literal project-relative paths and '.' for the "
                    "project root; do not inspect $HOME or other system locations. For native Haiku GUI apps, "
                    "write C++ using BApplication and BWindow, compile with g++, and link with -lbe. Do not "
                    "probe for bcc, bchk, bimg, getbeospath, or /boot/develop, and do not use -nostdlib unless "
                    "the project explicitly requires it. Treat command-not-found and linker diagnostics as "
                    "failures even if a compound shell command reports exit code 0. Writes and shell commands "
                    "require approval.", {}, {}});
                profile.api_key = api_key;
                auto provider = kairo::CreateProvider(
                    profile, 120, ChatGPTCredentialPath().string());
                kairo::Limits limits;
                kairo::AgentEngine engine(provider,
                    kairo::Workspace(project, limits.max_tool_output_bytes,
                                     {profile.api_key_environment}),
                    store, limits);
                engine_started = true;
                engine.Run(session, prompt, approval, events, cancellation_);
            } catch (const std::exception& error) {
                if (!engine_started) {
                    BMessage update(kEngineEvent);
                    update.AddInt32("type", static_cast<int32>(kairo::EventType::Error));
                    update.AddString("text", error.what());
                    update.AddString("session", session_id.c_str());
                    target.SendMessage(&update);
                }
            }
            BMessage done(kEngineEvent);
            done.AddInt32("type", static_cast<int32>(kairo::EventType::Completed));
            done.AddBool("worker_done", true);
            target.SendMessage(&done);
        });
    }

    void ReceiveEngineEvent(BMessage* message) {
        bool worker_done = false;
        message->FindBool("worker_done", &worker_done);
        if (worker_done) {
            ResolveAllApprovals(false);
            if (worker_.joinable()) worker_.join();
            WriteLog("INFO", "Run worker stopped");
            SetRunning(false);
            return;
        }
        int32 raw_type = 0;
        const char* text = nullptr;
        const char* session = nullptr;
        message->FindInt32("type", &raw_type);
        if (message->FindString("text", &text) != B_OK || !text) text = "";
        if (message->FindString("session", &session) != B_OK || !session) session = "";
        auto type = static_cast<kairo::EventType>(raw_type);
        if (type == kairo::EventType::SessionSaved && *session
            && std::strlen(session_->Text()) == 0)
            session_->SetText(session);
        if (type == kairo::EventType::TextDelta) {
            AppendTranscript(text, TranscriptStyle::Assistant);
        } else if (type == kairo::EventType::ToolStarted) {
            WriteLog("INFO", std::string("Tool started; name=") + text);
            status_->SetText((std::string("Running ") + text).c_str());
            AppendTranscript(std::string("\n\n[tool: ") + text + "]\n",
                             TranscriptStyle::Tool, true);
        }
        else if (type == kairo::EventType::ToolFinished || type == kairo::EventType::ToolDenied) {
            const bool denied = type == kairo::EventType::ToolDenied;
            WriteLog(denied ? "WARNING" : "INFO",
                denied ? "Tool action denied" : "Tool finished");
            AppendTranscript(denied ? "[denied]\n" : "[tool result]\n",
                             denied ? TranscriptStyle::Warning : TranscriptStyle::Tool, true);
            AppendTranscript(text, denied ? TranscriptStyle::Warning
                                          : TranscriptStyle::ToolOutput);
            AppendTranscript("\n\nKairo: ", TranscriptStyle::Assistant, true);
        }
        else if (type == kairo::EventType::Completed) {
            WriteLog("INFO", "Run completed");
            status_->SetText("Complete");
        }
        else if (type == kairo::EventType::Cancelled) {
            WriteLog("INFO", "Run cancelled");
            status_->SetText("Cancelled");
        }
        else if (type == kairo::EventType::Error) {
            WriteLog("ERROR", std::string("Engine error: ") + text);
            status_->SetText("Error");
            if (std::strncmp(text, "session not found:", 18) == 0)
                session_->SetText("");
            AppendTranscript("\nError: ", TranscriptStyle::Error, true);
            AppendTranscript(text, TranscriptStyle::Error);
        }
    }

    void ShowApproval(BMessage* message) {
        void* pointer = nullptr;
        const char* preview = nullptr;
        if (message->FindPointer("approval", &pointer) != B_OK || !pointer) return;
        if (message->FindString("preview", &preview) != B_OK || !preview)
            preview = "No action preview was supplied.";
        WriteLog("INFO", "Approval requested for a model-proposed action");
        BMessage* decision = new BMessage(kApprovalDecision);
        decision->AddPointer("approval", pointer);
        BAlert* alert = new BAlert("approval", preview, "Deny", "Approve", nullptr,
                                   B_WIDTH_AS_USUAL, B_WARNING_ALERT);
        alert->SetShortcut(0, B_ESCAPE);
        alert->Go(new BInvoker(decision, this));
    }

    void FinishApproval(BMessage* message) {
        void* pointer = nullptr;
        int32 which = 0;
        if (message->FindPointer("approval", &pointer) != B_OK || !pointer) return;
        message->FindInt32("which", &which);
        std::shared_ptr<PendingApproval> pending;
        {
            std::lock_guard<std::mutex> lock(approvals_mutex_);
            auto found = std::find_if(approvals_.begin(), approvals_.end(),
                [pointer](const std::shared_ptr<PendingApproval>& candidate) {
                    return candidate.get() == pointer;
                });
            if (found == approvals_.end()) return;
            pending = *found;
            approvals_.erase(found);
        }
        WriteLog(which == 1 ? "INFO" : "WARNING",
            which == 1 ? "Model-proposed action approved" : "Model-proposed action denied");
        pending->Resolve(which == 1);
    }

    void ResolveAllApprovals(bool approved) {
        std::lock_guard<std::mutex> lock(approvals_mutex_);
        for (const auto& pending : approvals_) pending->Resolve(approved);
        approvals_.clear();
    }

    void SetRunning(bool running) {
        send_->SetEnabled(!running);
        cancel_->SetEnabled(running);
        project_->SetEnabled(!running);
        choose_->SetEnabled(!running);
        session_->SetEnabled(!running);
        provider_->SetEnabled(!running);
        model_->SetEnabled(!running);
        approval_checkbox_->SetEnabled(!running);
        settings_->SetEnabled(!running);
        choose_project_item_->SetEnabled(!running);
        new_session_item_->SetEnabled(!running);
        settings_item_->SetEnabled(!running);
        send_item_->SetEnabled(!running);
        cancel_item_->SetEnabled(running);
        if (running) status_->SetText("Working");
    }

    BMenuBar* menu_bar_;
    BMenuItem* choose_project_item_;
    BMenuItem* new_session_item_;
    BMenuItem* focus_prompt_item_;
    BMenuItem* send_item_;
    BMenuItem* cancel_item_;
    BMenuItem* settings_item_;
    BTextControl* project_;
    BButton* choose_;
    BTextControl* session_;
    BButton* settings_;
    BMenuField* provider_;
    BPopUpMenu* provider_menu_;
    BMenuField* model_;
    BPopUpMenu* model_menu_;
    BCheckBox* approval_checkbox_;
    BTextView* transcript_;
    BTextView* prompt_;
    BStringView* status_;
    BButton* send_;
    BButton* cancel_;
    std::thread worker_;
    kairo::CancellationToken cancellation_;
    std::mutex approvals_mutex_;
    std::vector<std::shared_ptr<PendingApproval>> approvals_;
    BFilePanel* project_panel_ = nullptr;
    ProviderSettings provider_settings_;
    std::size_t selected_provider_index_ = 0;
    BMessenger settings_messenger_;
};

class KairoApplication : public BApplication {
public:
    KairoApplication() : BApplication(kApplicationSignature) {}
    void ReadyToRun() override {
        auto* window = new KairoWindow();
        window->Show();
    }
};

}  // namespace

int main() {
    KairoApplication application;
    application.Run();
    return 0;
}
