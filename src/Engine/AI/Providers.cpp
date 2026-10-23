// Providers.cpp: Claude (Anthropic Messages API), ChatGPT (OpenAI Chat Completions) and Gemini (Google generateContent) over the
// vendored libcurl (same build switch as FlyCloud). Each provider is a request builder + a response parser (pure, tested
// offline) plus one shared HTTPS POST. Adding another provider = a builder, a parser and a class here, one ProviderId value.
#ifdef _WIN32
#ifndef NOGDI
#define NOGDI
#endif
#ifndef NOUSER
#define NOUSER
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "../../../include/Engine/AI/AI.hpp"

#if FLYENGINE_ENABLE_FLYCLOUD

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>

namespace ai {

namespace {

using nlohmann::json;

std::once_flag g_curlInit;

size_t WriteBody(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

int ProgressCheck(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<std::atomic<bool>*>(clientp)->load() ? 1 : 0;
}

// One blocking HTTPS request: a POST when `payload` is given, else a GET. `headers` are full "Name: value" lines (they hold the
// API key: never logged). R is ChatResult or ModelListResult; both start as failures and carry the error text.
template <class R>
R Fetch(const std::string& url, const std::vector<std::string>& headers, const std::string* payload, const char* host,
        R (*parse)(long, const std::string&), const std::atomic<bool>* cancel = nullptr) {
    R r;
    std::call_once(g_curlInit, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "Could not initialize curl."; return r; }

    std::string body;
    char errbuf[CURL_ERROR_SIZE] = {0};
    struct curl_slist* list = nullptr;
    for (const std::string& h : headers) list = curl_slist_append(list, h.c_str());
    if (payload) list = curl_slist_append(list, "content-type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);              // called from worker threads: no SIGALRM-based timeouts
    if (payload) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload->c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload->size());
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");   // a key is never sent over plain http
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, payload ? 120L : 30L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    if (cancel) {                                   // Stop: curl asks this on every progress tick; non-zero aborts the transfer
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &ProgressCheck);
    }

    const CURLcode res = curl_easy_perform(curl);
    long code = 0;
    if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);

    if (res == CURLE_ABORTED_BY_CALLBACK) { r.error = "Stopped."; return r; }
    if (res != CURLE_OK) {
        r.error = std::string("Could not reach ") + host + " (" + (errbuf[0] ? errbuf : curl_easy_strerror(res)) + ").";
        return r;
    }
    return parse(code, body);
}

// Typed field readers: nlohmann's value() throws on a null or wrong-typed member, and these parsers run on worker threads.
std::string StrField(const json& o, const char* key, const std::string& def = {}) {
    if (o.is_object()) { auto it = o.find(key); if (it != o.end() && it->is_string()) return it->get<std::string>(); }
    return def;
}
bool BoolField(const json& o, const char* key, bool def) {
    if (o.is_object()) { auto it = o.find(key); if (it != o.end() && it->is_boolean()) return it->get<bool>(); }
    return def;
}
long long IntField(const json& o, const char* key, long long def) {
    if (o.is_object()) { auto it = o.find(key); if (it != o.end() && it->is_number()) return it->get<long long>(); }
    return def;
}

std::string UrlEscape(const std::string& s) {
    char* e = curl_easy_escape(nullptr, s.c_str(), (int)s.size());
    std::string out = e ? e : "";
    if (e) curl_free(e);
    return out;
}

// "error": { "message": ... } is the error shape of all three APIs.
std::string ErrorDetail(const json& j) {
    if (j.is_object() && j.contains("error") && j["error"].is_object() && j["error"].contains("message") && j["error"]["message"].is_string())
        return j["error"]["message"].get<std::string>();
    return {};
}

// Shared mapping of HTTP failures to something a person can act on.
std::string HttpErrorText(long code, const json& j) {
    std::string error;
    const std::string detail = ErrorDetail(j);
    switch (code) {
        case 400: error = detail.find("API key") != std::string::npos ? std::string("Invalid API key (400).") : "The request was rejected (400)."; break;
        case 401: error = "Invalid API key (401)."; break;
        case 403: error = "This key is not allowed to use that model or API (403)."; break;
        case 404: error = "Model not found (404): check the model name in Preferences > AI."; break;
        case 429: error = "Rate limited or out of quota (429): try again in a moment."; break;
        default: error = "API error " + std::to_string(code) + "."; break;
    }
    if (!detail.empty() && code != 401) error += " " + detail;
    return error;
}

ChatResult HttpError(long code, const json& j) { ChatResult r; r.error = HttpErrorText(code, j); return r; }
ModelListResult ModelsHttpError(long code, const json& j) { ModelListResult r; r.error = HttpErrorText(code, j); return r; }

// Follows a provider's paging cursor (a few pages at most) and merges the pages.
template <class PageFn>
ModelListResult ListPages(PageFn page, const std::string& key) {
    ModelListResult all;
    if (key.empty()) { all.error = "No API key set."; return all; }
    std::string cursor;
    for (int i = 0; i < 5; ++i) {
        ModelListResult r = page(cursor);
        if (!r.ok) { if (all.models.empty()) return r; break; }   // keep what the earlier pages gave
        all.models.insert(all.models.end(), r.models.begin(), r.models.end());
        cursor = r.next;
        if (cursor.empty()) break;
    }
    all.ok = true;
    return all;
}

class ClaudeProvider final : public IProvider {
public:
    const char* Name() const override { return "Claude"; }
    ChatResult Chat(const ChatRequest& req, const std::string& key) override {
        if (key.empty()) return { false, {}, "No API key set." };
        const std::string payload = BuildClaudeRequestJson(req);
        return Fetch<ChatResult>("https://api.anthropic.com/v1/messages", { "x-api-key: " + key, "anthropic-version: 2023-06-01" },
                                 &payload, "api.anthropic.com", &ParseClaudeResponse, req.cancel.get());
    }
    ModelListResult ListModels(const std::string& key) override {
        return ListPages([&](const std::string& cursor) {
            return Fetch<ModelListResult>("https://api.anthropic.com/v1/models?limit=1000" + (cursor.empty() ? std::string() : "&after_id=" + UrlEscape(cursor)),
                                          { "x-api-key: " + key, "anthropic-version: 2023-06-01" }, nullptr, "api.anthropic.com", &ParseClaudeModels);
        }, key);
    }
};

class OpenAIProvider final : public IProvider {
public:
    const char* Name() const override { return "ChatGPT"; }
    ChatResult Chat(const ChatRequest& req, const std::string& key) override {
        if (key.empty()) return { false, {}, "No API key set." };
        const std::string payload = BuildOpenAIRequestJson(req);
        return Fetch<ChatResult>("https://api.openai.com/v1/chat/completions", { "Authorization: Bearer " + key },
                                 &payload, "api.openai.com", &ParseOpenAIResponse, req.cancel.get());
    }
    ModelListResult ListModels(const std::string& key) override {
        return ListPages([&](const std::string&) {
            return Fetch<ModelListResult>("https://api.openai.com/v1/models", { "Authorization: Bearer " + key }, nullptr, "api.openai.com", &ParseOpenAIModels);
        }, key);
    }
};

class GeminiProvider final : public IProvider {
public:
    const char* Name() const override { return "Gemini"; }
    ChatResult Chat(const ChatRequest& req, const std::string& key) override {
        if (key.empty()) return { false, {}, "No API key set." };
        if (!IsSafeModelId(req.model)) return { false, {}, "That model name has characters that are not allowed." };
        // The key goes in a header, never in the URL, so it cannot end up in a log or a proxy's access list.
        const std::string payload = BuildGeminiRequestJson(req);
        return Fetch<ChatResult>("https://generativelanguage.googleapis.com/v1beta/models/" + req.model + ":generateContent",
                                 { "x-goog-api-key: " + key }, &payload, "generativelanguage.googleapis.com", &ParseGeminiResponse, req.cancel.get());
    }
    ModelListResult ListModels(const std::string& key) override {
        return ListPages([&](const std::string& cursor) {
            return Fetch<ModelListResult>("https://generativelanguage.googleapis.com/v1beta/models?pageSize=1000" + (cursor.empty() ? std::string() : "&pageToken=" + UrlEscape(cursor)),
                                          { "x-goog-api-key: " + key }, nullptr, "generativelanguage.googleapis.com", &ParseGeminiModels);
        }, key);
    }
};

} // namespace

// ---- Claude ----

std::string BuildClaudeRequestJson(const ChatRequest& req) {
    json j;
    j["model"] = req.model;
    j["max_tokens"] = req.maxTokens;
    json sys = json::array();
    if (!req.systemStatic.empty())
        sys.push_back({ { "type", "text" }, { "text", req.systemStatic }, { "cache_control", { { "type", "ephemeral" } } } });
    if (!req.systemDynamic.empty()) sys.push_back({ { "type", "text" }, { "text", req.systemDynamic } });
    if (!sys.empty()) j["system"] = sys;
    json msgs = json::array();
    for (const ChatMessage& m : req.messages) {
        if (m.images.empty()) { msgs.push_back({ { "role", m.user ? "user" : "assistant" }, { "content", m.text } }); continue; }
        json parts = json::array();                      // images first, then the words about them
        for (const Attachment& a : m.images)
            parts.push_back({ { "type", "image" }, { "source", { { "type", "base64" }, { "media_type", a.mime }, { "data", a.data } } } });
        parts.push_back({ { "type", "text" }, { "text", m.text } });
        msgs.push_back({ { "role", "user" }, { "content", parts } });
    }
    j["messages"] = msgs;
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

ChatResult ParseClaudeResponse(long httpCode, const std::string& body) {
    ChatResult r;
    const json j = json::parse(body, nullptr, false);
    if (httpCode != 200) return HttpError(httpCode, j);
    if (!j.is_object() || !j.contains("content") || !j["content"].is_array()) { r.error = "Unexpected response from the API."; return r; }
    for (const auto& part : j["content"])
        if (part.is_object() && StrField(part, "type") == "text" && part.contains("text") && part["text"].is_string())
            r.text += part["text"].get<std::string>();
    if (r.text.empty()) {
        // Say why: the new models can think before answering, and thinking spends the same output limit as the answer.
        const std::string stop = StrField(j, "stop_reason");
        std::string types;
        for (const auto& part : j["content"]) if (part.is_object()) types += (types.empty() ? "" : ", ") + StrField(part, "type", "?");
        if (stop == "max_tokens") {
            r.budget = true;
            r.error = "The model used its whole output limit thinking and never wrote an answer.";
        } else if (stop == "refusal") {
            r.error = "The model declined to answer this request.";
        } else {
            r.error = "The model returned no text (stop: " + (stop.empty() ? std::string("none") : stop) + ", content: " + (types.empty() ? std::string("empty") : types) + ").";
        }
        return r;
    }
    r.ok = true;
    return r;
}

// ---- OpenAI (Chat Completions) ----

std::string BuildOpenAIRequestJson(const ChatRequest& req) {
    json j;
    j["model"] = req.model;
    j["max_completion_tokens"] = req.maxTokens;   // max_tokens is refused by the reasoning models
    json msgs = json::array();
    std::string sys = req.systemStatic;           // static docs first: OpenAI caches the longest repeated prefix
    if (!req.systemDynamic.empty()) sys += (sys.empty() ? "" : "\n\n") + req.systemDynamic;
    if (!sys.empty()) msgs.push_back({ { "role", "system" }, { "content", sys } });
    for (const ChatMessage& m : req.messages) {
        if (m.images.empty()) { msgs.push_back({ { "role", m.user ? "user" : "assistant" }, { "content", m.text } }); continue; }
        json parts = json::array();
        parts.push_back({ { "type", "text" }, { "text", m.text } });
        for (const Attachment& a : m.images)
            parts.push_back({ { "type", "image_url" }, { "image_url", { { "url", "data:" + a.mime + ";base64," + a.data } } } });
        msgs.push_back({ { "role", "user" }, { "content", parts } });
    }
    j["messages"] = msgs;
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

ChatResult ParseOpenAIResponse(long httpCode, const std::string& body) {
    ChatResult r;
    const json j = json::parse(body, nullptr, false);
    if (httpCode != 200) return HttpError(httpCode, j);
    if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() || j["choices"].empty() || !j["choices"][0].is_object()) {
        r.error = "Unexpected response from the API.";
        return r;
    }
    const json& choice = j["choices"][0];
    if (choice.contains("message") && choice["message"].is_object()) {
        const json& msg = choice["message"];
        if (msg.contains("content") && msg["content"].is_string()) r.text = msg["content"].get<std::string>();
        else if (msg.contains("content") && msg["content"].is_array())
            for (const auto& part : msg["content"])
                if (part.is_object() && part.contains("text") && part["text"].is_string()) r.text += part["text"].get<std::string>();
        if (r.text.empty() && msg.contains("refusal") && msg["refusal"].is_string()) { r.error = "The model refused: " + msg["refusal"].get<std::string>(); return r; }
    }
    if (r.text.empty()) {
        const std::string why = StrField(choice, "finish_reason");
        r.budget = why == "length";
        r.error = r.budget ? "The model used its whole output limit thinking and never wrote an answer."
                           : "The model returned no text" + (why.empty() ? std::string(".") : " (finish: " + why + ").");
        return r;
    }
    r.ok = true;
    return r;
}

// ---- Gemini (generateContent) ----

std::string BuildGeminiRequestJson(const ChatRequest& req) {
    json j;
    std::string sys = req.systemStatic;
    if (!req.systemDynamic.empty()) sys += (sys.empty() ? "" : "\n\n") + req.systemDynamic;
    if (!sys.empty()) j["systemInstruction"] = { { "parts", json::array({ { { "text", sys } } }) } };
    json contents = json::array();
    for (const ChatMessage& m : req.messages) {
        json parts = json::array({ { { "text", m.text } } });
        for (const Attachment& a : m.images)
            parts.push_back({ { "inline_data", { { "mime_type", a.mime }, { "data", a.data } } } });
        contents.push_back({ { "role", m.user ? "user" : "model" }, { "parts", parts } });
    }
    j["contents"] = contents;
    j["generationConfig"] = { { "maxOutputTokens", req.maxTokens } };
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

ChatResult ParseGeminiResponse(long httpCode, const std::string& body) {
    ChatResult r;
    const json j = json::parse(body, nullptr, false);
    if (httpCode != 200) return HttpError(httpCode, j);
    if (!j.is_object()) { r.error = "Unexpected response from the API."; return r; }
    if (!j.contains("candidates") || !j["candidates"].is_array() || j["candidates"].empty()) {
        if (j.contains("promptFeedback") && j["promptFeedback"].is_object() && j["promptFeedback"].contains("blockReason"))
            r.error = "Gemini blocked the request: " + j["promptFeedback"]["blockReason"].dump();
        else r.error = "The model returned no answer.";
        return r;
    }
    const json& cand = j["candidates"][0];
    if (cand.is_object() && cand.contains("content") && cand["content"].is_object() && cand["content"].contains("parts") && cand["content"]["parts"].is_array())
        for (const auto& part : cand["content"]["parts"])
            if (part.is_object() && part.contains("text") && part["text"].is_string() && !BoolField(part, "thought", false))
                r.text += part["text"].get<std::string>();
    if (r.text.empty()) {
        const std::string why = StrField(cand, "finishReason");
        r.budget = why == "MAX_TOKENS";
        r.error = r.budget ? "The model used its whole output limit thinking and never wrote an answer."
                           : "The model returned no text" + (why.empty() ? std::string(".") : " (" + why + ").");
        return r;
    }
    r.ok = true;
    return r;
}

// ---- model lists (one page each) ----

ModelListResult ParseClaudeModels(long httpCode, const std::string& body) {
    ModelListResult r;
    const json j = json::parse(body, nullptr, false);
    if (httpCode != 200) return ModelsHttpError(httpCode, j);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) { r.error = "Unexpected response from the API."; return r; }
    for (const auto& m : j["data"]) {
        if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) continue;
        ModelInfo info;
        info.id = m["id"].get<std::string>();
        if (!IsSafeModelId(info.id)) continue;
        info.label = m.contains("display_name") && m["display_name"].is_string() ? m["display_name"].get<std::string>() : info.id;
        r.models.push_back(std::move(info));                     // the API already lists the newest first
    }
    if (BoolField(j, "has_more", false) && j.contains("last_id") && j["last_id"].is_string()) r.next = j["last_id"].get<std::string>();
    r.ok = true;
    return r;
}

ModelListResult ParseOpenAIModels(long httpCode, const std::string& body) {
    ModelListResult r;
    const json j = json::parse(body, nullptr, false);
    if (httpCode != 200) return ModelsHttpError(httpCode, j);
    if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) { r.error = "Unexpected response from the API."; return r; }
    for (const auto& m : j["data"]) {
        if (!m.is_object() || !m.contains("id") || !m["id"].is_string()) continue;
        ModelInfo info;
        info.id = m["id"].get<std::string>();
        if (!IsSafeModelId(info.id)) continue;
        info.label = info.id;
        info.chat = LooksLikeChatModel(info.id);
        info.created = IntField(m, "created", 0);
        r.models.push_back(std::move(info));
    }
    std::stable_sort(r.models.begin(), r.models.end(), [](const ModelInfo& a, const ModelInfo& b) { return a.created > b.created; });
    r.ok = true;
    return r;
}

ModelListResult ParseGeminiModels(long httpCode, const std::string& body) {
    ModelListResult r;
    const json j = json::parse(body, nullptr, false);
    if (httpCode != 200) return ModelsHttpError(httpCode, j);
    if (!j.is_object() || !j.contains("models") || !j["models"].is_array()) { r.error = "Unexpected response from the API."; return r; }
    for (const auto& m : j["models"]) {
        if (!m.is_object() || !m.contains("name") || !m["name"].is_string()) continue;
        ModelInfo info;
        info.id = m["name"].get<std::string>();
        if (info.id.rfind("models/", 0) == 0) info.id.erase(0, 7);
        if (!IsSafeModelId(info.id)) continue;
        info.label = m.contains("displayName") && m["displayName"].is_string() ? m["displayName"].get<std::string>() : info.id;
        if (m.contains("supportedGenerationMethods") && m["supportedGenerationMethods"].is_array()) {
            info.chat = false;
            for (const auto& method : m["supportedGenerationMethods"]) info.chat |= method.is_string() && method.get<std::string>() == "generateContent";
        }
        r.models.push_back(std::move(info));
    }
    if (j.contains("nextPageToken") && j["nextPageToken"].is_string()) r.next = j["nextPageToken"].get<std::string>();
    r.ok = true;
    return r;
}

namespace {
std::function<std::unique_ptr<IProvider>(ProviderId)> g_factory;
}

void SetProviderFactoryForTesting(std::function<std::unique_ptr<IProvider>(ProviderId)> factory) { g_factory = std::move(factory); }

std::unique_ptr<IProvider> MakeProvider(ProviderId id) {
    if (g_factory) return g_factory(id);
    switch (id) {
        case ProviderId::Claude: return std::make_unique<ClaudeProvider>();
        case ProviderId::OpenAI: return std::make_unique<OpenAIProvider>();
        case ProviderId::Gemini: return std::make_unique<GeminiProvider>();
    }
    return nullptr;
}

} // namespace ai

#else

namespace ai {
std::string BuildClaudeRequestJson(const ChatRequest&) { return {}; }
ChatResult ParseClaudeResponse(long, const std::string&) { return { false, {}, "AI is not available in this build." }; }
std::string BuildOpenAIRequestJson(const ChatRequest&) { return {}; }
ChatResult ParseOpenAIResponse(long, const std::string&) { return { false, {}, "AI is not available in this build." }; }
std::string BuildGeminiRequestJson(const ChatRequest&) { return {}; }
ChatResult ParseGeminiResponse(long, const std::string&) { return { false, {}, "AI is not available in this build." }; }
ModelListResult ParseClaudeModels(long, const std::string&) { return {}; }
ModelListResult ParseOpenAIModels(long, const std::string&) { return {}; }
ModelListResult ParseGeminiModels(long, const std::string&) { return {}; }
std::unique_ptr<IProvider> MakeProvider(ProviderId) { return nullptr; }
void SetProviderFactoryForTesting(std::function<std::unique_ptr<IProvider>(ProviderId)>) {}
}

#endif
