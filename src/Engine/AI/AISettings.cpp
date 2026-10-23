// AISettings.cpp: ai.cfg under the machine config dir. Never inside a project, so it is never exported or committed.
#include "../../../include/Engine/AI/AI.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/U8Path.hpp"
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#if FLYENGINE_ENABLE_FLYCLOUD
#include <nlohmann/json.hpp>
#endif
#include <fstream>

namespace ai {

namespace {
Settings g_settings;
std::string g_pathOverride;

std::string ConfigFile() {
    if (!g_pathOverride.empty()) return g_pathOverride;
    const std::string dir = platform::ConfigDir();
    return dir.empty() ? std::string() : dir + "/ai.cfg";
}
}

Settings& GetSettings() { return g_settings; }
void SetConfigPathForTesting(const std::string& path) { g_pathOverride = path; }

std::string DefaultModel(ProviderId id) {
    switch (id) {
        case ProviderId::Claude: return "claude-sonnet-5-5";
        case ProviderId::OpenAI: return "gpt-6-astra";
        case ProviderId::Gemini: return "gemini-3.8-flash";
    }
    return {};
}

const char* ProviderName(ProviderId id) {
    switch (id) {
        case ProviderId::Claude: return "Claude (Anthropic)";
        case ProviderId::OpenAI: return "ChatGPT (OpenAI)";
        case ProviderId::Gemini: return "Gemini (Google)";
    }
    return "?";
}

const char* ProviderShortName(ProviderId id) {
    switch (id) {
        case ProviderId::Claude: return "Claude";
        case ProviderId::OpenAI: return "ChatGPT";
        case ProviderId::Gemini: return "Gemini";
    }
    return "?";
}

const char* ProviderEnvVar(ProviderId id) {
    switch (id) {
        case ProviderId::Claude: return "ANTHROPIC_API_KEY";
        case ProviderId::OpenAI: return "OPENAI_API_KEY";
        case ProviderId::Gemini: return "GEMINI_API_KEY";
    }
    return "";
}

int ProviderIconIndex(ProviderId id) {
    switch (id) {
        case ProviderId::Claude: return 0;
        case ProviderId::OpenAI: return 2;
        case ProviderId::Gemini: return 1;
    }
    return 0;
}

std::string EnvKey(ProviderId id) {
    const char* e = std::getenv(ProviderEnvVar(id));
    if ((!e || !*e) && id == ProviderId::Gemini) e = std::getenv("GOOGLE_API_KEY");
    return e && *e ? std::string(e) : std::string();
}

bool KeyFromEnvironment() { return !EnvKey(g_settings.provider).empty(); }

std::string EffectiveKey() {
    const std::string env = EnvKey(g_settings.provider);
    return env.empty() ? g_settings.key[Index(g_settings.provider)] : env;
}

std::string& SelectedKey() { return g_settings.key[Index(g_settings.provider)]; }
std::string& SelectedModel() { return g_settings.model[Index(g_settings.provider)]; }

bool IsSafeModelId(const std::string& id) {
    if (id.empty() || id.size() > 100) return false;
    for (char c : id)
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == ':')) return false;
    return true;
}

namespace {
std::vector<ModelInfo> g_models[kProviderCount];
long long g_modelsFetched[kProviderCount] = {};
std::string g_modelsStatus[kProviderCount];
}

const std::vector<ModelInfo>& Models(ProviderId id) { return g_models[Index(id)]; }

std::vector<ModelInfo> ModelChoices(ProviderId id) {
    std::vector<ModelInfo> out;
    for (const ModelInfo& m : g_models[Index(id)])
        if (m.chat || g_settings.showAllModels) out.push_back(m);
    return out;
}

std::string ModelLabel(ProviderId id, const std::string& modelId) {
    for (const ModelInfo& m : g_models[Index(id)])
        if (m.id == modelId) return m.label.empty() ? m.id : m.label;
    return modelId;
}

void SetModels(ProviderId id, std::vector<ModelInfo> models) {
    g_models[Index(id)] = std::move(models);
    g_modelsFetched[Index(id)] = (long long)std::time(nullptr);
}

long long ModelsFetchedAt(ProviderId id) { return g_modelsFetched[Index(id)]; }

bool ModelsStale(ProviderId id) {
    return g_models[Index(id)].empty() || (long long)std::time(nullptr) - g_modelsFetched[Index(id)] > 24 * 3600;
}

const std::string& ModelsStatus(ProviderId id) { return g_modelsStatus[Index(id)]; }
void SetModelsStatus(ProviderId id, const std::string& s) { g_modelsStatus[Index(id)] = s; }

// OpenAI's /v1/models lists everything the key can reach and has no capability field, so chat models are told apart by name.
bool LooksLikeChatModel(const std::string& id) {
    static const char* const notChat[] = { "embedding", "whisper", "tts", "dall-e", "moderation", "image", "audio", "realtime",
                                           "transcribe", "sora", "davinci", "babbage", "instruct", "computer-use" };
    for (const char* bad : notChat) if (id.find(bad) != std::string::npos) return false;
    return true;
}

bool Available() {
#if FLYENGINE_ENABLE_FLYCLOUD
    return true;
#else
    return false;
#endif
}

bool Active() { return Available() && g_settings.enabled && !EffectiveKey().empty(); }

std::string MaskKey(const std::string& key) {
    if (key.empty()) return "(not set)";
    if (key.size() <= 12) return std::string(key.size(), '*');
    return key.substr(0, 7) + "..." + key.substr(key.size() - 4);
}

void Load() {
    g_settings = Settings{};
    if (platform::IsEmbedded()) return;          // an exported game never gets the editor's AI
    LoadModelCache();
    const std::string path = ConfigFile();
    if (path.empty()) return;
    std::ifstream in(U8Path(path));
    for (std::string line; std::getline(in, line);) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
        if (k == "enabled") g_settings.enabled = (v == "1");
        else if (k == "show_all_models") g_settings.showAllModels = (v == "1");
        else if (k == "mode_script") g_settings.modeScript = (v == "1");
        else if (k == "mode_build") g_settings.modeBuild = (v == "1");
        else if (k == "mode_ask") g_settings.modeAsk = (v == "1");
        else if (k == "provider") {
            for (int i = 0; i < kProviderCount; ++i)
                if (v == ProviderShortName(static_cast<ProviderId>(i))) g_settings.provider = static_cast<ProviderId>(i);
        } else {
            for (int i = 0; i < kProviderCount; ++i) {
                std::string prefix = ProviderShortName(static_cast<ProviderId>(i));
                for (char& c : prefix) c = (char)std::tolower((unsigned char)c);
                if (k == prefix + ".key") g_settings.key[i] = v;
                else if (k == prefix + ".model" && !v.empty()) g_settings.model[i] = v;
            }
        }
    }
}

void Save() {
    if (platform::IsEmbedded()) return;
    const std::string path = ConfigFile();
    if (path.empty()) return;
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(U8Path(path).parent_path(), ec);
    {   // create it owner-only before the key goes in
        std::ofstream touch(U8Path(path), std::ios::app);
    }
    fs::permissions(U8Path(path), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    std::ofstream out(U8Path(path), std::ios::trunc);
    if (!out) return;
    out << "enabled=" << (g_settings.enabled ? 1 : 0) << "\n";
    out << "show_all_models=" << (g_settings.showAllModels ? 1 : 0) << "\n";
    out << "mode_script=" << (g_settings.modeScript ? 1 : 0) << "\nmode_build=" << (g_settings.modeBuild ? 1 : 0)
        << "\nmode_ask=" << (g_settings.modeAsk ? 1 : 0) << "\n";
    out << "provider=" << ProviderShortName(g_settings.provider) << "\n";
    for (int i = 0; i < kProviderCount; ++i) {
        std::string prefix = ProviderShortName(static_cast<ProviderId>(i));
        for (char& c : prefix) c = (char)std::tolower((unsigned char)c);
        out << prefix << ".key=" << g_settings.key[i] << "\n";
        out << prefix << ".model=" << g_settings.model[i] << "\n";
    }
}

#if FLYENGINE_ENABLE_FLYCLOUD

namespace {
std::string CachePath() {
    const std::string cfg = ConfigFile();
    return cfg.empty() ? std::string() : (U8Path(cfg).parent_path() / "ai_models.json").string();
}
std::string ProviderKeyName(int i) {
    std::string n = ProviderShortName(static_cast<ProviderId>(i));
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    return n;
}
}

void SaveModelCache() {
    if (platform::IsEmbedded()) return;
    const std::string path = CachePath();
    if (path.empty()) return;
    nlohmann::json j;
    for (int i = 0; i < kProviderCount; ++i) {
        nlohmann::json arr = nlohmann::json::array();
        for (const ModelInfo& m : g_models[i]) arr.push_back({ { "id", m.id }, { "label", m.label }, { "chat", m.chat }, { "created", m.created } });
        j[ProviderKeyName(i)] = { { "fetched", g_modelsFetched[i] }, { "models", arr } };
    }
    std::error_code ec;
    std::filesystem::create_directories(U8Path(path).parent_path(), ec);
    std::ofstream out(U8Path(path), std::ios::trunc | std::ios::binary);
    if (out) out << j.dump(1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void LoadModelCache() {
    for (int i = 0; i < kProviderCount; ++i) { g_models[i].clear(); g_modelsFetched[i] = 0; g_modelsStatus[i].clear(); }
    const std::string path = CachePath();
    if (path.empty()) return;
    std::ifstream in(U8Path(path), std::ios::binary);
    if (!in) return;
    const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    try {
    for (int i = 0; i < kProviderCount; ++i) {
        const std::string key = ProviderKeyName(i);
        if (!j.contains(key) || !j[key].is_object() || !j[key].contains("models") || !j[key]["models"].is_array()) continue;
        g_modelsFetched[i] = j[key].value("fetched", 0LL);
        for (const auto& m : j[key]["models"]) {
            if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) continue;
            ModelInfo info;
            info.id = m["id"].get<std::string>();
            info.label = m.value("label", info.id);
            info.chat = m.value("chat", true);
            info.created = m.value("created", 0LL);
            if (IsSafeModelId(info.id)) g_models[i].push_back(std::move(info));   // a hand-edited cache can't smuggle odd names in
        }
    }
    } catch (const nlohmann::json::exception&) {   // a hand-edited cache with wrong-typed fields: use what was read, refetch later
    }
}

#else

void SaveModelCache() {}
void LoadModelCache() {}

#endif

} // namespace ai
