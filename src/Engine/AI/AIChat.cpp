// AIChat.cpp: the chat service. A request runs on a detached worker thread (the editor never blocks on the network) and
// its result waits in a queue until Update() hands it to the main thread, the same shape as fcloud::Update().
// Workers never touch the scene. The open chat is saved to disk (ChatStore.cpp) after every message.
#include "../../../include/Engine/AI/AI.hpp"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>

namespace ai {

namespace {

enum class Job { Chat, Test, Models };

struct Done { Job job; unsigned generation; ChatResult result; ModelListResult models; ProviderId provider = ProviderId::Claude; unsigned requestId = 0; };

std::vector<Message> g_history;
std::string g_chatId;       // "" until the open chat has a first message
std::string g_chatTitle;
std::string g_testStatus;
std::atomic<bool> g_busy{ false };
std::atomic<bool> g_modelsBusy{ false };
std::atomic<unsigned> g_requestId{ 0 };    // the one request whose result is still wanted; Stop() moves it on
std::shared_ptr<std::atomic<bool>> g_cancel;   // its cancel token (main thread only)
Job g_activeJob = Job::Chat;
std::atomic<unsigned> g_generation{ 0 };   // bumped whenever the open chat changes, so a stale reply is dropped
// Finished jobs waiting for Update(). Never destroyed: a worker still downloading when the editor quits may finish after
// static destructors have run, and must not lock a dead mutex.
struct Inbox { std::mutex mutex; std::vector<Done> done; };
Inbox& TheInbox() { static Inbox* inbox = new Inbox(); return *inbox; }

void Launch(Job job, ChatRequest req, std::string key, ProviderId id) {
    g_busy = true;
    g_activeJob = job;
    g_cancel = std::make_shared<std::atomic<bool>>(false);
    req.cancel = g_cancel;
    const unsigned gen = g_generation.load();
    const unsigned requestId = ++g_requestId;
    std::thread([job, req = std::move(req), key = std::move(key), id, gen, requestId]() {
        ChatResult res;
        try {                                       // an exception escaping a detached thread would terminate the editor
            if (auto p = MakeProvider(id)) {
                res = p->Chat(req, key);
                // Spent its whole output limit before the answer (thinking): one more try with double the room.
                if (!res.ok && res.budget && req.maxTokens < 32000 && !req.cancel->load()) {
                    ChatRequest bigger = req;
                    bigger.maxTokens = std::min(32000, req.maxTokens * 2);
                    res = p->Chat(bigger, key);
                }
            } else res.error = "AI is not available in this build.";
        } catch (...) { res = ChatResult{}; res.error = "The request failed unexpectedly."; }
        Inbox& inbox = TheInbox();
        std::lock_guard<std::mutex> lock(inbox.mutex);
        inbox.done.push_back({ job, gen, std::move(res), {}, id, requestId });
    }).detach();
}

// The conversation as the provider gets it: error notices are UI only; a user message carries its text files inline and its images as parts.
std::vector<ChatMessage> RequestMessages() {
    std::vector<ChatMessage> out;
    for (const Message& m : g_history) {
        if (m.error) continue;
        ChatMessage cm;
        cm.user = m.user;
        cm.text = m.user ? ComposeUserText(m.text, m.attachments) : m.text;
        for (const Attachment& a : m.attachments) if (a.image && !a.data.empty()) cm.images.push_back(a);
        out.push_back(std::move(cm));
    }
    return out;
}

// Writes the open chat to disk (a chat with no messages is never saved).
void SaveCurrent() {
    if (g_history.empty()) return;
    if (g_chatId.empty()) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        g_chatId = "c" + std::to_string(ms);
        std::string t = g_history.front().text;
        for (char& ch : t) if (ch == '\n' || ch == '\r') ch = ' ';
        if (t.size() > 48) { size_t cut = 48; while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut; t.resize(cut); t += "..."; }   // never cut a UTF-8 character
        g_chatTitle = t;
    }
    ChatInfo meta;
    meta.id = g_chatId;
    meta.title = g_chatTitle;
    meta.updated = (long long)std::time(nullptr);
    SaveChatFile(meta, g_history);
}

} // namespace

const std::vector<Message>& History() { return g_history; }
bool Busy() { return g_busy; }
const std::string& TestStatus() { return g_testStatus; }
const std::string& CurrentChatId() { return g_chatId; }
const std::string& CurrentChatTitle() { return g_chatTitle; }

void Stop() {
    if (!g_busy) return;
    if (g_cancel) g_cancel->store(true);        // the worker aborts its transfer at the next progress tick
    ++g_requestId;                              // ...and whatever it still delivers is ignored
    g_busy = false;                             // so the UI is free at once
    if (g_activeJob == Job::Test) { g_testStatus = "Stopped."; return; }
    if (!g_history.empty() && g_history.back().user) g_history.push_back({ false, true, "Stopped.", true });
}

void ClearHistory() {
    g_history.clear();
    g_chatId.clear();
    g_chatTitle.clear();
    ++g_generation;
}

void NewChat() { ClearHistory(); }

bool OpenChat(const std::string& id) {
    if (g_busy) return false;
    ChatInfo meta;
    std::vector<Message> msgs;
    if (!LoadChatFile(id, meta, msgs)) return false;
    g_history = std::move(msgs);
    g_chatId = meta.id;
    g_chatTitle = meta.title;
    ++g_generation;
    return true;
}

void DeleteChat(const std::string& id) {
    DeleteChatFile(id);
    if (id == g_chatId) ClearHistory();
}

void Send(const std::string& userText, const std::string& dynamicContext, std::vector<Attachment> attachments) {
    const bool blank = userText.find_first_not_of(" \t\r\n") == std::string::npos;
    if (!Active() || g_busy || (blank && attachments.empty())) return;
    Message m;
    m.user = true;
    m.text = blank ? "Please look at what I attached." : userText;
    m.attachments = std::move(attachments);
    g_history.push_back(std::move(m));
    SaveCurrent();
    ChatRequest req;
    req.systemStatic = EngineSystemPrompt();
    req.systemDynamic = dynamicContext;
    req.model = SelectedModel();
    req.messages = RequestMessages();
    Launch(Job::Chat, std::move(req), EffectiveKey(), GetSettings().provider);
}

bool RetryLast(const std::string& dynamicContext) {
    if (!Active() || g_busy || g_history.size() < 2 || !g_history.back().error || !g_history[g_history.size() - 2].user) return false;
    g_history.pop_back();                                       // the error notice goes; the question stays
    ChatRequest req;
    req.systemStatic = EngineSystemPrompt();
    req.systemDynamic = dynamicContext;
    req.model = SelectedModel();
    req.messages = RequestMessages();
    Launch(Job::Chat, std::move(req), EffectiveKey(), GetSettings().provider);
    return true;
}

void TestConnection() {
    if (!Available() || g_busy || EffectiveKey().empty()) return;
    g_testStatus = "Testing...";
    ChatRequest req;
    req.model = SelectedModel();
    if (!IsSafeModelId(req.model) && GetSettings().provider == ProviderId::Gemini) { g_testStatus = "That model name has characters that are not allowed."; return; }
    req.maxTokens = 256;                        // reasoning models spend tokens thinking before the answer
    req.messages.push_back({ true, "Reply with the single word OK." });
    Launch(Job::Test, std::move(req), EffectiveKey(), GetSettings().provider);
}

void PushAssistantMessageForTesting(const std::string& text) { g_history.push_back({ false, false, text }); }

void ResetTestStatus() { g_testStatus.clear(); }

bool ModelsLoading() { return g_modelsBusy; }

void RefreshModels() {
    if (!Available() || g_modelsBusy) return;
    const std::string key = EffectiveKey();
    const ProviderId id = GetSettings().provider;
    if (key.empty()) { SetModelsStatus(id, "Enter an API key first."); return; }
    g_modelsBusy = true;
    SetModelsStatus(id, "Loading models...");
    std::thread([key, id]() {
        ModelListResult res;
        try {
            if (auto p = MakeProvider(id)) res = p->ListModels(key);
            else res.error = "AI is not available in this build.";
        } catch (...) { res = ModelListResult{}; res.error = "The request failed unexpectedly."; }
        Inbox& inbox = TheInbox();
        std::lock_guard<std::mutex> lock(inbox.mutex);
        inbox.done.push_back({ Job::Models, 0, {}, std::move(res), id });
    }).detach();
}

void MaybeRefreshModels() {
    if (Active() && ModelsStale(GetSettings().provider)) RefreshModels();
}

void Update() {
    if (!g_busy && !g_modelsBusy) return;                        // nothing in flight (also the whole story while AI is disabled)
    std::vector<Done> done;
    {
        Inbox& inbox = TheInbox();
        std::lock_guard<std::mutex> lock(inbox.mutex);
        done.swap(inbox.done);
    }
    for (Done& d : done) {
        if (d.job == Job::Models) {
            g_modelsBusy = false;
            if (!d.models.ok) { SetModelsStatus(d.provider, d.models.error); continue; }
            SetModels(d.provider, std::move(d.models.models));
            const auto choices = ModelChoices(d.provider);
            SetModelsStatus(d.provider, std::to_string(Models(d.provider).size()) + " models" +
                                        (Models(d.provider).size() != choices.size() ? " (" + std::to_string(choices.size()) + " for chat)" : ""));
            // Still on the built-in fallback and it is not in the provider's list: move to the provider's first chat model.
            std::string& chosen = GetSettings().model[Index(d.provider)];
            bool present = false;
            for (const ModelInfo& m : Models(d.provider)) present |= m.id == chosen;
            if (!present && chosen == DefaultModel(d.provider) && !choices.empty()) chosen = choices.front().id;
            SaveModelCache();
            Save();
            continue;
        }
        if (d.requestId != g_requestId.load()) continue;   // a request that was stopped: its late result is dropped
        g_busy = false;
        if (d.job == Job::Test) {
            g_testStatus = d.result.ok ? "OK" : d.result.error;
        } else if (d.generation == g_generation.load()) {
            if (d.result.ok) g_history.push_back({ false, false, d.result.text });
            else g_history.push_back({ false, true, d.result.error });
            SaveCurrent();
        }
    }
}

} // namespace ai
