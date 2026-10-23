// AI.hpp: the optional AI assistant. Off by default: while Settings::enabled is false nothing here touches the
// network, the config file is never created, and the editor shows no chat entry. The provider sits behind IProvider so
// Gemini / ChatGPT can be added later as one more class plus one ProviderId.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ai {

enum class ProviderId { Claude = 0, OpenAI = 1, Gemini = 2 };
constexpr int kProviderCount = 3;
inline int Index(ProviderId id) { return static_cast<int>(id); }

std::string DefaultModel(ProviderId id);

struct Settings {
    bool enabled = false;
    ProviderId provider = ProviderId::Claude;
    std::string key[kProviderCount];             // as typed in Preferences > AI; the provider's env var overrides it
    std::string model[kProviderCount];
    bool modeScript = false;                     // the + menu: what the AI may produce (all off = explain only)
    bool modeBuild = false;
    bool modeAsk = true;                         // the AI may ask a question instead of guessing
    bool showAllModels = false;                  // list models that are not chat models too (embeddings, speech, images...)
    Settings() { for (int i = 0; i < kProviderCount; ++i) model[i] = DefaultModel(static_cast<ProviderId>(i)); }
};

Settings& GetSettings();
void Load();                                     // reads <ConfigDir>/ai.cfg (missing file = defaults, i.e. disabled)
void Save();                                     // writes it with owner-only permissions where the OS supports that
void SetConfigPathForTesting(const std::string& path);   // empty = the real <ConfigDir>/ai.cfg

const char* ProviderName(ProviderId id);        // "Claude (Anthropic)"
const char* ProviderShortName(ProviderId id);   // "Claude"
const char* ProviderEnvVar(ProviderId id);      // ANTHROPIC_API_KEY / OPENAI_API_KEY / GEMINI_API_KEY
int ProviderIconIndex(ProviderId id);           // index into assets/EditorIcons/AI: 0 claude, 1 gemini, 2 chatgpt
std::string EnvKey(ProviderId id);              // the env var's value ("" when unset; Gemini also accepts GOOGLE_API_KEY)
std::string EffectiveKey();                      // env var if set, else the stored key for the selected provider
bool KeyFromEnvironment();                       // for the selected provider
std::string& SelectedKey();                      // the stored key of the selected provider
std::string& SelectedModel();                    // the model string of the selected provider
bool IsSafeModelId(const std::string& id);       // letters, digits, . _ - : only (it ends up in a URL for Gemini)

// ---- the provider's own model list: fetched from its API, cached on disk (<ConfigDir>/ai_models.json) ----
struct ModelInfo {
    std::string id;                              // what the API wants in the request
    std::string label;                           // display name when the provider gives one, else the id
    bool chat = true;                            // can answer chat requests (OpenAI lists embeddings, speech, images too)
    long long created = 0;
};
const std::vector<ModelInfo>& Models(ProviderId id);          // the cached list (empty until fetched)
std::vector<ModelInfo> ModelChoices(ProviderId id);           // Models() minus non-chat ones unless Settings::showAllModels
std::string ModelLabel(ProviderId id, const std::string& modelId);   // the cached label, or the id as typed
void SetModels(ProviderId id, std::vector<ModelInfo> models);        // replaces the cache and stamps the time
long long ModelsFetchedAt(ProviderId id);                     // epoch seconds, 0 = never
bool ModelsStale(ProviderId id);                              // never fetched, or older than a day
void RefreshModels();                                         // async: fetch the selected provider's list (needs a key)
void MaybeRefreshModels();                                    // RefreshModels() when the list is stale and a key is present
void SetModelsStatus(ProviderId id, const std::string& status);
const std::string& ModelsStatus(ProviderId id);               // "" / "Loading models..." / "42 models" / the error
bool ModelsLoading();
void SaveModelCache();
void LoadModelCache();
bool LooksLikeChatModel(const std::string& openaiModelId);    // OpenAI's list has no capability field: a name check
bool Available();                                // this build can talk to a provider (needs libcurl)
bool Active();                                   // Available() && enabled && a key is present
std::string MaskKey(const std::string& key);

// ---- attachments: a file or an image sent with a message ----
// A text file is put into the message as a fenced block; an image goes to the provider as an image part. Limits keep a request sane.
struct Attachment {
    std::string name;                            // the file name, shown in the chat
    bool image = false;
    std::string mime;                            // image: image/png or image/jpeg
    std::string data;                            // text file: its contents. image: base64 (never saved with the chat)
    int width = 0, height = 0;                   // image: the size that is sent
    std::vector<unsigned char> thumb;            // image: a small RGBA preview for the UI
    int thumbW = 0, thumbH = 0;
};
constexpr size_t kMaxTextAttachment = 200 * 1024;
constexpr int kMaxAttachments = 6;
constexpr int kMaxImageSide = 1568;              // larger images are scaled down before sending
bool LoadAttachment(const std::string& path, bool asImage, Attachment& out, std::string& error);
std::string Base64Encode(const std::string& bytes);
// The message as the model sees it: what you typed, plus each text file in a fenced block, plus a note for an image that was in the
// chat but is no longer available (images are not kept on disk).
std::string ComposeUserText(const std::string& typed, const std::vector<Attachment>& attachments);

// ---- provider interface ----
struct ChatMessage { bool user = true; std::string text; std::vector<Attachment> images; };   // images: only those with data
struct ChatRequest {
    std::string systemStatic;                    // engine docs + rules: identical every turn, so it is cached
    std::string systemDynamic;                   // project / selection, changes per turn
    std::vector<ChatMessage> messages;
    std::string model;
    int maxTokens = 16000;
    std::shared_ptr<std::atomic<bool>> cancel;   // set by Stop(): the transfer is aborted at its next progress tick
};
struct ChatResult {
    bool ok = false;
    std::string text;
    std::string error;
    bool budget = false;                         // empty because the model spent its whole output limit (thinking): worth one retry with more
};

struct ModelListResult { bool ok = false; std::vector<ModelInfo> models; std::string next; std::string error; };

class IProvider {
public:
    virtual ~IProvider() = default;
    virtual ChatResult Chat(const ChatRequest& req, const std::string& apiKey) = 0;   // blocking: call off the main thread
    virtual struct ModelListResult ListModels(const std::string& apiKey) = 0;          // blocking as well
    virtual const char* Name() const = 0;
};
std::unique_ptr<IProvider> MakeProvider(ProviderId id);
void SetProviderFactoryForTesting(std::function<std::unique_ptr<IProvider>(ProviderId)> factory);   // null = the real providers   // null when this build has no HTTP support

// Pure helpers behind ClaudeProvider (unit-testable without the network).
std::string BuildClaudeRequestJson(const ChatRequest& req);
ChatResult ParseClaudeResponse(long httpCode, const std::string& body);
std::string BuildOpenAIRequestJson(const ChatRequest& req);
ChatResult ParseOpenAIResponse(long httpCode, const std::string& body);
std::string BuildGeminiRequestJson(const ChatRequest& req);
ChatResult ParseGeminiResponse(long httpCode, const std::string& body);
// One page of the provider's model list; `next` is the cursor for the following page ("" when that was the last).
ModelListResult ParseClaudeModels(long httpCode, const std::string& body);
ModelListResult ParseOpenAIModels(long httpCode, const std::string& body);
ModelListResult ParseGeminiModels(long httpCode, const std::string& body);

// ---- docs the model is given ----
std::string ScriptTemplate();                    // the example script in the system prompt (tests compile it)
std::string EngineSystemPrompt();                // role + rules + the embedded Flyengine docs

// ---- reply parsing: fenced blocks the user can Apply ----
struct Segment {
    enum Kind { Text, Console, Script, Code, Question } kind = Text;
    std::string text;                            // Question: the question itself
    std::vector<std::string> options;            // Question: the choices (clicking one sends it as the answer)
    std::string name;                            // Script: file name such as "Spinner.cpp"
};
// `partial` is for text still being revealed: an unfinished fence shows as a code block instead of plain text.
std::vector<Segment> ParseReply(const std::string& reply, bool partial = false);
// What the + menu allows, as instructions appended to each request.
std::string ModeInstructions(bool script, bool build, bool ask);
std::string DeriveScriptName(const std::string& code);                     // "Spinner.cpp" from FLY_SCRIPT(Spinner), or ""
std::string NormalizeScriptName(const std::string& raw, const std::string& code);   // base name, .cpp added, never a path
bool IsSafeScriptName(const std::string& name);  // plain "Name.cpp", no separators, no "..", identifier characters only

// ---- AI-written scripts are NOT part of the project until it is saved ----
// Apply writes the script so it can compile, but like model imports and new script stubs it is temporary: a journal in
// <project>/.ai_pending records what was there before. Saving the project (any path that calls SaveProjectFile) commits;
// Discard, closing the editor, or the next launch after a crash puts the project back (a new file is deleted, an
// overwritten one is restored). A file edited by hand since the AI wrote it is never touched: it becomes yours.
enum class PendingOutcome { NotPending, Reverted, KeptEdited };
bool WritePendingScript(const std::string& projectFolder, const std::string& name, const std::string& code, std::string& error);
bool IsScriptPending(const std::string& projectFolder, const std::string& name);
std::vector<std::string> PendingScripts(const std::string& projectFolder);
PendingOutcome RevertPendingScript(const std::string& projectFolder, const std::string& name);
int RevertAllPendingScripts(const std::string& projectFolder);   // returns how many files were reverted (edited ones are kept)
void CommitPendingScripts(const std::string& projectFolder);     // the project was saved: keep them all
bool IsAllowedConsoleLine(const std::string& line);   // refuses cloud / account commands

// ---- chat service (one request at a time, worker thread, drained by Update on the main thread) ----
struct Message { bool user = true; bool error = false; std::string text; bool stopped = false; std::vector<Attachment> attachments; };
const std::vector<Message>& History();
bool Busy();
void Send(const std::string& userText, const std::string& dynamicContext, std::vector<Attachment> attachments = {});
void Stop();                                     // abandon the running chat request now (the transfer is aborted; its late result is ignored)
bool RetryLast(const std::string& dynamicContext);   // after an error: ask again for the same message (no duplicate)
void ClearHistory();                             // empties the open chat (a saved copy stays on disk); drops a reply in flight

// ---- saved chats: one JSON file per chat in <ConfigDir>/chats, written after every message ----
struct ChatInfo { std::string id; std::string title; long long updated = 0; };
std::vector<ChatInfo> ListChats();               // newest first
const std::string& CurrentChatId();              // "" until the open chat has its first message
const std::string& CurrentChatTitle();
void NewChat();                                  // start an empty chat (not refused while busy: the old reply is dropped)
bool OpenChat(const std::string& id);            // false while a request is running, or if the file is bad
void DeleteChat(const std::string& id);          // deleting the open chat starts a new one
bool IsValidChatId(const std::string& id);       // ids become file names: lowercase letters, digits, '_' only
bool SaveChatFile(const ChatInfo& meta, const std::vector<Message>& msgs);
bool LoadChatFile(const std::string& id, ChatInfo& meta, std::vector<Message>& msgs);
void DeleteChatFile(const std::string& id);
void SetChatsDirForTesting(const std::string& dir);   // empty = <ConfigDir>/chats
void PushAssistantMessageForTesting(const std::string& text);   // appends a reply as if it had arrived (UI tests)
void TestConnection();
void ResetTestStatus();                          // forget the last connection test (the provider or key changed)
const std::string& TestStatus();                 // connection test, see TestConnection()                 // "" / "Testing..." / "OK" / error text
void Update();                                   // once per frame; returns at once when disabled

} // namespace ai
