// ui_ai.cpp: the AI assistant window. Only reachable (top-bar entry) while ai::Active(); replies can carry code blocks
// with an Apply button, and nothing runs until the user clicks it.
#include "ui_internal.hpp"
#include "../../../include/Engine/AI/AI.hpp"
#include "../../../include/Engine/U8Path.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstring>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>

namespace ui {

namespace {
std::function<void()> g_beforeApply;
char g_input[4096] = "";
bool g_scrollToEnd = false;
size_t g_lastCount = 0;
int g_fadeIdx = -1;        // history index of the message fading in
int g_revealIdx = -1;      // history index of the reply being typed out
double g_revealStart = 0.0;
std::map<std::pair<int, int>, int> g_answers;   // question block -> the option chosen
double g_fadeStart = 0.0;
bool g_wasBusy = false;
std::vector<ai::ChatInfo> g_chatList;   // refreshed whenever the Chats view is opened
bool g_showChatList = false;           // the panel shows the list of saved chats instead of the open chat
std::string g_confirmDelete;           // chat id whose x was clicked once

// Watching the compile that an applied script triggers, so the chat says whether it worked instead of leaving you guessing.
struct WatchItem { std::pair<int, int> key; std::string name; };
struct BuildWatch {
    bool active = false, sawBuilding = false;
    std::vector<WatchItem> items;                // every script written by the same Apply
    double started = 0.0;
};
struct Change { std::pair<int, int> key; ai::Segment seg; };   // one block of the latest reply that Apply would write or run
std::set<std::pair<int, int>> g_applied;       // blocks already applied (or tried): they no longer count as pending changes
double g_flashUntil = 0.0;                     // the bar says "N Changes applied!" until then
int g_flashCount = 0;
// Files and images waiting to go with the next message (added with the + menu), their preview textures, and the last error.
std::vector<ai::Attachment> g_attach;
std::vector<Texture2D> g_attachThumb;          // parallel to g_attach; id 0 for a text file
bool g_attachAsImage = false;                  // what the file dialog in flight was asked for
std::string g_attachError;
double g_attachErrorUntil = 0.0;
BuildWatch g_watch;
std::vector<std::string> g_pending;   // AI scripts not yet saved with the project (cached: the journal is a file)
double g_pendingChecked = -1.0;
std::map<std::pair<int, int>, std::string> g_buildErrors;   // block -> the compiler errors, ready to send back to the model
double g_busyStart = 0.0;
// Per code block (message index, segment index) outcome text, and whether an overwrite is awaiting confirmation.
std::map<std::pair<int, int>, std::string> g_applyResult;

// A different chat is now open: forget per-message UI state (Apply results are keyed by message position).
void ResetChatView() {
    g_applyResult.clear();
    g_applied.clear();
    g_flashUntil = 0.0;
    g_buildErrors.clear();
    g_watch.active = false;
    g_lastCount = ai::History().size();   // so the opened chat's last message does not fade in
    g_fadeIdx = -1;
    g_revealIdx = -1;
    g_answers.clear();
    g_scrollToEnd = true;
}

std::string FormatWhen(long long t) {
    char buf[32];
    const std::time_t tt = (std::time_t)t;
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    std::strftime(buf, sizeof buf, "%b %d, %H:%M", &tmv);
    return buf;
}

void ClearAttachments() {
    for (Texture2D& t : g_attachThumb) if (t.id != 0) UnloadTexture(t);
    g_attachThumb.clear();
    g_attach.clear();
}

void RemoveAttachment(size_t i) {
    if (i >= g_attach.size()) return;
    if (g_attachThumb[i].id != 0) UnloadTexture(g_attachThumb[i]);
    g_attachThumb.erase(g_attachThumb.begin() + (long)i);
    g_attach.erase(g_attach.begin() + (long)i);
}

void AddAttachment(const std::string& path, bool asImage) {
    if ((int)g_attach.size() >= ai::kMaxAttachments) {
        g_attachError = "At most " + std::to_string(ai::kMaxAttachments) + " attachments per message.";
        g_attachErrorUntil = ImGui::GetTime() + 6.0;
        return;
    }
    ai::Attachment a;
    std::string error;
    if (!ai::LoadAttachment(path, asImage, a, error)) {
        g_attachError = error;
        g_attachErrorUntil = ImGui::GetTime() + 6.0;
        return;
    }
    Texture2D tex{};
    if (a.image && !a.thumb.empty()) {
        Image im{ a.thumb.data(), a.thumbW, a.thumbH, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };   // copied to the GPU; the vector stays ours
        tex = LoadTextureFromImage(im);
    }
    g_attachThumb.push_back(tex);
    g_attach.push_back(std::move(a));
}

std::string SceneContext() {
    std::string c = "Current editor state:\n";
    const project::Info& p = project::GetCurrentProject();
    c += "- Project: " + (p.name.empty() ? std::string("(none)") : p.name) + "\n";
    const auto& sel = GetSelection();
    if (sel.empty()) c += "- Selection: nothing\n";
    else {
        c += "- Selection:";
        int n = 0;
        for (ScatteredObject* o : sel) { if (o && n++ < 12) c += " " + o->GetName() + ","; }
        c.pop_back();
        c += "\n";
    }
    if (g_sceneObjects) {                           // the names already taken, so new objects do not clash and edits can refer to them
        c += "- Objects in the scene (" + std::to_string(g_sceneObjects->size()) + "):";
        int n = 0;
        for (const ScatteredObject* o : *g_sceneObjects) { if (o && n++ < 80) c += " " + o->GetName() + ","; }
        if (n == 0) c += " none,";
        if ((int)g_sceneObjects->size() > 80) c += " ...";
        else c.pop_back();
        c += "\n";
    }
    const ai::Settings& s = ai::GetSettings();
    c += "\n" + ai::ModeInstructions(s.modeScript, s.modeBuild, s.modeAsk);
    return c;
}

void ApplyConsole(const std::string& block, std::string& result) {
    int ran = 0, failed = 0;
    std::string log;
    bool snapshotted = false;
    size_t pos = 0;
    while (pos <= block.size()) {
        size_t nl = block.find('\n', pos);
        if (nl == std::string::npos) nl = block.size();
        std::string line = block.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        if (!ai::IsAllowedConsoleLine(line)) { log += "Skipped (not allowed from AI): " + line + "\n"; ++failed; continue; }
        if (!snapshotted && g_beforeApply) { g_beforeApply(); snapshotted = true; }   // one undo step for the whole block
        std::string out;
        const bool ok = console::RunLine(line, &out);
        ++ran; failed += !ok;
        log += (ok ? "> " : "! ") + line + (out.empty() ? "" : "  ->  " + out) + "\n";
    }
    result = std::to_string(ran) + " command(s) run" + (failed ? ", " + std::to_string(failed) + " failed or skipped" : "") +
             (snapshotted ? " (Ctrl+Z undoes them; nothing is saved until you save the project)" : "") + "\n" + log;
}

std::string BaseName(const std::string& path) {
    const size_t at = path.find_last_of("/\\");
    return at == std::string::npos ? path : path.substr(at + 1);
}

// Saving the project (Ctrl+S, File > Save, Export) commits the AI scripts from elsewhere, so look again twice a second.
void RefreshPending(bool force = false) {
    const double now = ImGui::GetTime();
    if (!force && now - g_pendingChecked < 0.5) return;
    g_pendingChecked = now;
    g_pending = ai::PendingScripts(project::GetCurrentProject().path);
}

bool IsPendingScript(const std::string& name) {
    for (const std::string& n : g_pending) if (n == name) return true;
    return false;
}

// Called every frame while the panel is drawn: turns the editor's script build state into a line under each written block.
void PollBuildWatch() {
    if (!g_watch.active) return;
    const double now = ImGui::GetTime();
    if (g_scriptBuildState == ScriptBuildState::Building) g_watch.sawBuilding = true;
    if (g_watch.sawBuilding && g_scriptBuildState == ScriptBuildState::Succeeded) {
        for (const WatchItem& it : g_watch.items) {
            const std::string cls = it.name.substr(0, it.name.size() - 4);
            g_applyResult[it.key] = "Compiled OK. " + cls + " is listed in the Explorer; press Play to run it (or type: run " + cls + ").\n"
                                    "Not saved yet: Ctrl+S keeps it in the project; closing the editor without saving removes it.";
        }
        g_watch.active = false;
        g_scrollToEnd = true;
    } else if (g_watch.sawBuilding && g_scriptBuildState == ScriptBuildState::Failed) {
        std::string errors;
        int n = 0;
        for (const ScriptDiagnostic& d : g_scriptDiagnostics) {
            if (d.severity != ScriptDiagnostic::Severity::Error) continue;
            if (++n > 20) break;
            errors += (d.file.empty() ? std::string("error") : BaseName(d.file) + ":" + std::to_string(d.line)) + ": " + d.message + "\n";
        }
        if (errors.empty()) errors = "The build failed (no compiler output was captured).\n";
        for (const WatchItem& it : g_watch.items) {
            g_buildErrors[it.key] = errors;           // the errors belong to the whole build: every block offers to send them back
            g_applyResult[it.key] = "Wrote " + it.name + " (not saved), but the build did not compile. The previous scripts keep running.\n" + errors;
        }
        g_watch.active = false;
        g_scrollToEnd = true;
    } else if (!g_watch.sawBuilding && now - g_watch.started > 8.0) {
        for (const WatchItem& it : g_watch.items)
            g_applyResult[it.key] = "Wrote " + it.name + " (not saved), but no build started. Is a C++ compiler installed (clang++ or g++)?";
        g_watch.active = false;
    }
}

// Writes one script block as an unsaved change (see PendingChanges.cpp). The rebuild is requested once for the whole batch.
bool WriteScriptBlock(const std::string& name, const std::string& code, std::string& result) {
    if (!ai::IsSafeScriptName(name)) { result = "Refused: '" + name + "' is not a plain Name.cpp file name."; return false; }
    const project::Info& p = project::GetCurrentProject();
    if (p.path.empty()) { result = "No project is open."; return false; }
    std::string err;
    if (!ai::WritePendingScript(p.path, name, code, err)) { result = err; return false; }
    result = "Wrote Scripts/" + name + " (not saved to the project yet). Compiling...";
    return true;
}

// What the Apply bar counts: the Script / Console blocks of the newest finished reply that are allowed by the + menu and not yet applied.
std::vector<Change> CollectChanges() {
    std::vector<Change> out;
    const auto& h = ai::History();
    if (h.empty() || ai::Busy()) return out;
    const int li = (int)h.size() - 1;
    const ai::Message& m = h[li];
    if (m.user || m.error || li == g_revealIdx) return out;      // not while it is still being typed out
    const auto segs = ai::ParseReply(m.text);
    for (int si = 0; si < (int)segs.size(); ++si) {
        const ai::Segment& sg = segs[si];
        const bool allowed = sg.kind == ai::Segment::Script ? ai::GetSettings().modeScript
                           : sg.kind == ai::Segment::Console ? ai::GetSettings().modeBuild : false;
        if (!allowed || g_applied.count({ li, si })) continue;
        out.push_back({ { li, si }, sg });
    }
    return out;
}

// The bar's Apply: writes every script (one rebuild for all of them) and runs every console block, each as its own result line.
void ApplyChanges(const std::vector<Change>& changes) {
    BuildWatch w;
    int done = 0;
    for (const Change& c : changes) {
        g_applied.insert(c.key);                 // tried once: a block that fails shows why instead of keeping the bar up forever
        std::string& result = g_applyResult[c.key];
        if (c.seg.kind == ai::Segment::Console) { ApplyConsole(c.seg.text, result); ++done; }
        else if (WriteScriptBlock(c.seg.name, c.seg.text, result)) { w.items.push_back({ c.key, c.seg.name }); g_buildErrors.erase(c.key); ++done; }
    }
    if (!w.items.empty()) {
        std::string out;
        console::RunLine("rebuild", &out);
        w.active = true;
        w.started = ImGui::GetTime();
        g_watch = w;
        RefreshPending(true);
    }
    if (done > 0) { g_flashCount = done; g_flashUntil = ImGui::GetTime() + 2.5; }
    g_scrollToEnd = true;
}
}

// ---- reply text: a small markdown subset (headings, bullets, **bold**, `code`) with word wrapping ----
enum class RunStyle { Normal, Bold, Code };
struct Run { std::string text; RunStyle style; };

static std::vector<Run> ParseInline(const std::string& s) {
    std::vector<Run> out;
    std::string cur;
    bool bold = false, code = false;
    auto flush = [&] {
        if (!cur.empty()) out.push_back({ cur, code ? RunStyle::Code : bold ? RunStyle::Bold : RunStyle::Normal });
        cur.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') { flush(); code = !code; }
        else if (!code && s.compare(i, 2, "**") == 0) { flush(); bold = !bold; ++i; }
        else cur += s[i];
    }
    flush();
    return out;
}

// Draws one logical line wrapped to the panel width, with the runs styled. `base` is the normal text colour.
static void DrawRichLine(const std::string& line, float indent, ImVec4 base) {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight() + 3.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float x = indent, y = 0.0f;
    for (const Run& run : ParseInline(line)) {
        ImVec4 col = base;
        if (run.style == RunStyle::Bold) col = ImVec4(1.0f, 1.0f, 1.0f, base.w);
        if (run.style == RunStyle::Code) col = ImVec4(0.96f, 0.76f, 0.48f, base.w);
        size_t i = 0;
        while (i < run.text.size()) {
            size_t j = run.text.find(' ', i);
            j = j == std::string::npos ? run.text.size() : j;
            size_t k = j;
            while (k < run.text.size() && run.text[k] == ' ') ++k;       // the word plus the spaces after it
            const std::string word = run.text.substr(i, k - i);
            const float fullBare = ImGui::CalcTextSize(run.text.substr(i, j - i).c_str()).x;
            // A word wider than a whole line (a long identifier or path) is broken between characters instead of clipped.
            std::vector<std::string> parts;
            const float room = std::max(10.0f, width - indent);
            if (fullBare > room) {
                const std::string full = run.text.substr(i, j - i);
                std::string cur;
                for (size_t c = 0; c < full.size();) {
                    size_t n = 1;
                    while (c + n < full.size() && (full[c + n] & 0xC0) == 0x80) ++n;   // keep UTF-8 characters whole
                    const std::string ch = full.substr(c, n);
                    if (!cur.empty() && ImGui::CalcTextSize((cur + ch).c_str()).x > room) { parts.push_back(cur); cur.clear(); }
                    cur += ch;
                    c += n;
                }
                parts.push_back(cur + run.text.substr(j, k - j));
            } else {
                parts.push_back(word);
            }
            for (size_t pi = 0; pi < parts.size(); ++pi) {
                const std::string& part = parts[pi];
                const float w = ImGui::CalcTextSize(part.c_str()).x;
                size_t trail = part.size();
                while (trail > 0 && part[trail - 1] == ' ') --trail;
                const float bare = ImGui::CalcTextSize(part.substr(0, trail).c_str()).x;
                if ((x + bare > width && x > indent) || (pi > 0 && x > indent)) { x = indent; y += lineH; }
                if (run.style == RunStyle::Code)
                    dl->AddRectFilled(ImVec2(origin.x + x - 2.0f, origin.y + y), ImVec2(origin.x + x + bare + 2.0f, origin.y + y + lineH - 1.0f),
                                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.08f)), 3.0f);
                dl->AddText(ImVec2(origin.x + x, origin.y + y + 1.0f), ImGui::GetColorU32(col), part.c_str());
                x += w;
            }
            i = k;
        }
    }
    ImGui::Dummy(ImVec2(width, y + lineH));
}

static void DrawMarkdown(const std::string& text, ImVec4 base) {
    size_t pos = 0;
    const float lineH = ImGui::GetTextLineHeight() + 3.0f;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && line[lead] == ' ') ++lead;
        const std::string body = line.substr(lead);
        const float nest = std::min<size_t>(lead / 2, 3) * 12.0f;
        if (body.empty()) { ImGui::Dummy(ImVec2(0.0f, 5.0f)); continue; }
        if (body == "---" || body == "***") { ImGui::Separator(); continue; }
        size_t h = 0;
        while (h < body.size() && body[h] == '#') ++h;
        if (h > 0 && h <= 4 && body.size() > h && body[h] == ' ') {
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
            DrawRichLine(body.substr(h + 1), 0.0f, theme::AccentVec());
            continue;
        }
        const ImVec2 at = ImGui::GetCursorScreenPos();
        if (body.size() > 2 && (body[0] == '-' || body[0] == '*') && body[1] == ' ') {
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + nest + 5.0f, at.y + lineH * 0.5f), 2.2f, ImGui::GetColorU32(base));
            DrawRichLine(body.substr(2), nest + 14.0f, base);
            continue;
        }
        size_t d = 0;
        while (d < body.size() && std::isdigit((unsigned char)body[d])) ++d;
        if (d > 0 && d + 1 < body.size() && body[d] == '.' && body[d + 1] == ' ') {
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + nest, at.y + 1.0f), ImGui::GetColorU32(base), body.substr(0, d + 1).c_str());
            DrawRichLine(body.substr(d + 2), nest + 20.0f, base);
            continue;
        }
        DrawRichLine(body, nest, base);
    }
}

// An icon that pulses while the assistant works. `tint` is the accent colour; alpha follows a sine of time.
static void PulsingIcon(ImTextureID icon, float size, double now) {
    if (!icon) return;
    ImVec4 tint = theme::AccentVec();
    tint.w = 0.55f + 0.45f * (0.5f + 0.5f * std::sin((float)now * 5.0f));
    ImGui::ImageWithBg(icon, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0), tint);
}

// The providers' logos (assets/EditorIcons/AI/) and the status icons (AI/panelicons/), kept as they are: not recoloured like
// the editor's black silhouettes. A missing file just gives no icon.
static ImTextureID AIIcon(int index) {
    constexpr int kCount = 9;
    static Texture2D tex[kCount];
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        constexpr const char* files[kCount] = { "claude.png", "gemini.png", "chatgpt.png", "panelicons/thinking.png",
                                                "panelicons/reading.png", "panelicons/writing.png", "panelicons/testing.png",
                                                "panelicons/chats.png", "panelicons/build.png" };
        for (int i = 0; i < kCount; ++i) {
            const std::string path = platform::ResolveAsset(std::string("assets/EditorIcons/AI/") + files[i]);
            if (path.empty()) continue;
            Image img = LoadImage(path.c_str());
            if (img.data == nullptr) continue;
            tex[i] = LoadTextureFromImage(img);
            UnloadImage(img);
        }
    }
    if (index < 0 || index >= kCount || tex[index].id == 0) return 0;
    return static_cast<ImTextureID>(GetTextureImGuiId(tex[index]));
}
ImTextureID AIProviderIcon(int index) { return index >= 0 && index <= 2 ? AIIcon(index) : 0; }
ImTextureID AIPanelIcon(int phase) { return phase >= 0 && phase <= 5 ? AIIcon(3 + phase) : 0; }   // thinking, reading, writing, testing, chats, build

// The model picker shared by the AI panel and Preferences. The list is the provider's own (fetched, cached), not a fixed one.
bool DrawAIModelCombo(const char* id, float width) {
    const ai::ProviderId pid = ai::GetSettings().provider;
    std::string& current = ai::SelectedModel();
    static char filter[64] = "";
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    ImGui::SetNextWindowSizeConstraints(ImVec2(280.0f, 0.0f), ImVec2(420.0f, 420.0f));
    if (ImGui::BeginCombo(id, ai::ModelLabel(pid, current).c_str())) {
        if (ImGui::IsWindowAppearing()) {
            filter[0] = 0;
            ai::MaybeRefreshModels();                       // first look, or a day old: ask the provider
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##modelfilter", "Search models...", filter, sizeof filter);
        if (ImGui::SmallButton("Refresh")) ai::RefreshModels();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", ai::ModelsLoading() ? "Loading..." : ai::ModelsStatus(pid).c_str());
        ImGui::Separator();

        std::string needle = filter;
        for (char& c : needle) c = (char)std::tolower((unsigned char)c);
        auto matches = [&](const ai::ModelInfo& m) {
            if (needle.empty()) return true;
            std::string hay = m.id + " " + m.label;
            for (char& c : hay) c = (char)std::tolower((unsigned char)c);
            return hay.find(needle) != std::string::npos;
        };
        const auto choices = ai::ModelChoices(pid);
        ImGui::BeginChild("##modellist", ImVec2(0.0f, 240.0f), false);
        bool listed = false;
        for (const ai::ModelInfo& m : choices) {
            if (!matches(m)) continue;
            listed = true;
            ImGui::PushID(m.id.c_str());
            if (ImGui::Selectable(m.label.c_str(), m.id == current) && m.id != current) { current = m.id; changed = true; }
            if (m.label != m.id) { ImGui::SameLine(); ImGui::TextDisabled("%s", m.id.c_str()); }
            ImGui::PopID();
        }
        if (choices.empty()) {
            ImGui::TextWrapped("No list yet. Press Refresh (it needs an API key), or type a model ID under Preferences > AI.");
            ImGui::Selectable(current.c_str(), true);
        } else if (!listed) {
            ImGui::TextDisabled("No model matches.");
        }
        ImGui::EndChild();
        ImGui::EndCombo();
    }
    if (changed) ai::Save();
    return changed;
}

void AIAttachForTesting(const std::string& path, bool asImage) { AddAttachment(path, asImage); }

void SetAIBeforeApplyHook(std::function<void()> hook) { g_beforeApply = std::move(hook); }

// The saved chats as cards: click one to open it (back to the chat view), x then "Delete?" to remove it.
void DrawChatListView() {
    ImGui::BeginChild("##chatlistview", ImVec2(0.0f, 0.0f), false);
    if (g_chatList.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        DrawMarkdown("No saved chats yet.\n\nChats are saved on this computer as you go. Press **+** to start a new one.", ImVec4(0.62f, 0.63f, 0.67f, 1.0f));
    }
    for (size_t i = 0; i < g_chatList.size(); ++i) {
        const ai::ChatInfo c = g_chatList[i];
        ImGui::PushID(c.id.c_str());
        const bool open = c.id == ai::CurrentChatId();
        const float w = ImGui::GetContentRegionAvail().x - (ImGui::GetScrollMaxY() > 0.0f ? ImGui::GetStyle().ScrollbarSize : 0.0f);
        const float delW = 52.0f;
        const float th = ImGui::CalcTextSize(c.title.c_str(), nullptr, false, w - delW - 16.0f).y;
        const float h = th + ImGui::GetTextLineHeight() + 18.0f;
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool sure = g_confirmDelete == c.id;
        ImGui::BeginDisabled(ai::Busy());
        ImGui::InvisibleButton("##card", ImVec2(w - delW, h));
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
        const bool clicked = ImGui::IsItemClicked();
        ImGui::EndDisabled();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(open ? theme::AccentVec(0.22f) : ImVec4(1, 1, 1, hovered ? 0.12f : 0.06f)), 5.0f);
        if (open) dl->AddRect(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(theme::AccentVec(0.7f)), 5.0f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 8.0f, at.y + 5.0f), ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), c.title.c_str(), nullptr, w - delW - 16.0f);
        dl->AddText(ImVec2(at.x + 8.0f, at.y + 8.0f + th), ImGui::GetColorU32(ImVec4(0.6f, 0.6f, 0.65f, 1.0f)), FormatWhen(c.updated).c_str());
        // The delete button sits at the card's right edge.
        ImGui::SetCursorScreenPos(ImVec2(at.x + w - delW + 2.0f, at.y + 5.0f));
        if (ImGui::SmallButton(sure ? "Delete?" : "x")) {
            if (sure) {
                const bool wasOpen = open;
                ai::DeleteChat(c.id);
                if (wasOpen) ResetChatView();
                g_chatList = ai::ListChats();
                g_confirmDelete.clear();
                ImGui::PopID();
                break;
            }
            g_confirmDelete = c.id;
        }
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h + 4.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));          // ImGui wants an item after a cursor jump
        if (clicked && !ai::Busy() && ai::OpenChat(c.id)) { ResetChatView(); g_showChatList = false; }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// The bar above the message box: "3 Changes" and an Apply button; after Apply it says "3 Changes applied!" for a moment and goes away.
void DrawChangesBar(const std::vector<Change>& changes, bool flashing) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float h = ImGui::GetFrameHeight() + 4.0f;
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const int count = flashing ? g_flashCount : (int)changes.size();
    const ImVec4 tint = flashing ? ImVec4(0.30f, 0.80f, 0.45f, 0.22f) : theme::AccentVec(0.20f);
    dl->AddRectFilled(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(tint), 5.0f);
    dl->AddRect(at, ImVec2(at.x + w, at.y + h), ImGui::GetColorU32(flashing ? ImVec4(0.30f, 0.80f, 0.45f, 0.7f) : theme::AccentVec(0.5f)), 5.0f);

    float x = at.x + 8.0f;
    if (ImTextureID icon = AIPanelIcon(flashing ? 3 : 2)) {
        dl->AddImage(icon, ImVec2(x, at.y + (h - 14.0f) * 0.5f), ImVec2(x + 14.0f, at.y + (h + 14.0f) * 0.5f));
        x += 20.0f;
    }
    char label[64];
    std::snprintf(label, sizeof label, flashing ? "%d Change%s applied!" : "%d Change%s", count, count == 1 ? "" : "s");
    dl->AddText(ImVec2(x, at.y + (h - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), label);

    bool hovered = false;
    if (!flashing) {
        ImGui::SetCursorScreenPos(ImVec2(at.x + w - 62.0f, at.y + 2.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, theme::AccentVec(0.55f));
        if (ImGui::Button("Apply##changes", ImVec2(58.0f, h - 4.0f))) ApplyChanges(changes);
        ImGui::PopStyleColor();
        hovered = ImGui::IsItemHovered();
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y));
    ImGui::InvisibleButton("##changesbar", ImVec2(w - (flashing ? 0.0f : 64.0f), h));   // hover target for the list of what changes
    if ((hovered || ImGui::IsItemHovered()) && !flashing) {
        namespace fs = std::filesystem;
        ImGui::BeginTooltip();
        for (const Change& c : changes) {
            if (c.seg.kind == ai::Segment::Console) { ImGui::TextUnformatted("Console commands (Build)"); continue; }
            std::error_code ec;
            const bool exists = fs::exists(U8Path(project::GetCurrentProject().path) / "Scripts" / c.seg.name, ec);
            ImGui::Text("%s  %s", c.seg.name.c_str(), exists ? "(replaces the existing script)" : "(new)");
        }
        ImGui::TextDisabled("Not saved to the project until you save it.");
        ImGui::EndTooltip();
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + h));
    ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
}

Rectangle GetAIPanelBounds() { return GetPanelBounds(PanelId::AI); }

void DrawImGuiAIPanel() {
    const Rectangle rec = GetAIPanelBounds();
    if (rec.width <= 0.0f) return;
    ImGui::SetNextWindowPos(ImVec2(rec.x, rec.y));
    ImGui::SetNextWindowSize(ImVec2(rec.width, rec.height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##AIPanel", nullptr, flags);
    PollBuildWatch();
    {   // a file or image chosen in the + menu's dialog
        std::string picked;
        if (platform::PollDialogResult({ platform::DialogPurpose::AIAttach }, picked) && !picked.empty()) AddAttachment(picked, g_attachAsImage);
    }

    // The title strip doubles as the layout's drag handle (drag to move the panel, right-click to close it).
    if (ImTextureID icon = AIProviderIcon(ai::ProviderIconIndex(ai::GetSettings().provider))) { ImGui::Image(icon, ImVec2(18.0f, 18.0f)); ImGui::SameLine(); }
    ImGui::TextColored(theme::AccentVec(), "AI ASSISTANT");
    ImGui::Separator();
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY(), kPanelTitleH));

    if (!ai::Active()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Add your %s API key to start chatting.", ai::ProviderShortName(ai::GetSettings().provider));
        if (ImGui::Button("Open AI settings")) ShowPreferences(true, 1);
        ImGui::End();
        return;
    }

    // Two views of the panel: the open chat, or the list of saved chats. The + starts a new chat.
    {
        const float newW = 26.0f, gap2 = ImGui::GetStyle().ItemSpacing.x;
        const float tabW = (ImGui::GetContentRegionAvail().x - newW - gap2 * 2.0f) * 0.5f;
        auto tab = [&](const char* label, bool selected, ImTextureID icon) {
            ImGui::PushStyleColor(ImGuiCol_Button, selected ? theme::AccentVec(0.30f) : ImVec4(1.0f, 1.0f, 1.0f, 0.06f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, selected ? theme::AccentVec(0.40f) : ImVec4(1.0f, 1.0f, 1.0f, 0.12f));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const bool clicked = ImGui::Button(label, ImVec2(tabW, 0.0f));
            ImGui::PopStyleColor(2);
            if (icon) {                                               // the icon sits left of the label inside the tab
                const float lw = ImGui::CalcTextSize(label, nullptr, true).x;
                const float x = at.x + (tabW - lw - 20.0f) * 0.5f;
                ImGui::GetWindowDrawList()->AddImage(icon, ImVec2(x, at.y + 4.0f), ImVec2(x + 16.0f, at.y + 20.0f));
            }
            return clicked;
        };
        if (tab("Chat", !g_showChatList, 0)) g_showChatList = false;
        ImGui::SameLine();
        if (tab("   Chats", g_showChatList, AIPanelIcon(4))) { g_showChatList = true; g_chatList = ai::ListChats(); g_confirmDelete.clear(); }
        ImGui::SameLine();
        ImGui::BeginDisabled(ai::Busy());                              // a new chat mid-request would drop the reply
        if (ImGui::Button("+##newchat", ImVec2(newW, 0.0f))) { ai::NewChat(); ResetChatView(); g_showChatList = false; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("New chat");
    }
    if (g_showChatList) { DrawChatListView(); ImGui::End(); return; }

    RefreshPending();
    if (!g_pending.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%d unsaved script%s", (int)g_pending.size(), g_pending.size() == 1 ? "" : "s");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scripts the AI wrote are not part of the project until you save it (Ctrl+S).\nClosing the editor without saving removes them.");
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 64.0f);
        if (ImGui::SmallButton("Discard")) {
            ai::RevertAllPendingScripts(project::GetCurrentProject().path);
            std::string out;
            console::RunLine("rebuild", &out);
            g_buildErrors.clear();
            g_watch.active = false;
            RefreshPending(true);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove every unsaved AI script from the project");
    }

    // The message box starts as one line and grows with what you type (word-wrapped), up to a share of the panel;
    // the chat above it gives up that space. Sized from the text as of the previous frame.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const float boxMin = lineH + style.FramePadding.y * 2.0f;
    const float boxMax = std::max(boxMin, std::min(lineH * 8.0f + style.FramePadding.y * 2.0f, rec.height * 0.35f));
    float textH = lineH;
    if (g_input[0]) {
        textH = ImGui::CalcTextSize(g_input, nullptr, false, std::max(20.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - style.ItemSpacing.x - style.FramePadding.x * 2.0f - style.ScrollbarSize)).y;   // assume the scrollbar: a line too many beats a clipped first line
        const size_t len = std::strlen(g_input);
        if (g_input[len - 1] == '\n') textH += lineH;       // a trailing newline is an empty last line
    }
    const float boxH = std::clamp(textH + style.FramePadding.y * 2.0f, boxMin, boxMax);
    const std::vector<Change> changes = CollectChanges();
    const bool flashing = ImGui::GetTime() < g_flashUntil;
    const bool showBar = flashing || !changes.empty();
    const float barH = showBar ? ImGui::GetFrameHeight() + 4.0f + style.ItemSpacing.y : 0.0f;
    const bool attachError = ImGui::GetTime() < g_attachErrorUntil;
    const float attachH = (float)g_attach.size() * (std::max(ImGui::GetFrameHeight(), 26.0f) + style.ItemSpacing.y) + (attachError ? ImGui::GetTextLineHeightWithSpacing() * 2.0f : 0.0f);
    const bool anyChip = ai::GetSettings().modeScript || ai::GetSettings().modeBuild;
    const float inputH = boxH + ImGui::GetFrameHeight() + style.ItemSpacing.y * 2.0f + (anyChip ? ImGui::GetFrameHeight() : 0.0f) + barH + attachH;   // box + the model / Send row under it
    ImGui::BeginChild("##aihistory", ImVec2(0.0f, -inputH), true);
    const auto& hist = ai::History();
    const double now = ImGui::GetTime();
    if (hist.size() > g_lastCount) {                                  // the newest message fades in; a reply is typed out
        g_fadeIdx = (int)hist.size() - 1;
        g_fadeStart = now;
        if (!hist.back().user && !hist.back().error) { g_revealIdx = g_fadeIdx; g_revealStart = now; }
    }
    const ImVec4 text(0.84f, 0.85f, 0.88f, 1.0f);
    if (hist.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        DrawMarkdown("Ask how Flyengine works, or what to build.\n\nReplies can include code blocks with an **Apply** button. Nothing runs until you click it.",
                     ImVec4(0.62f, 0.63f, 0.67f, 1.0f));
    }
    for (int mi = 0; mi < (int)hist.size(); ++mi) {
        const ai::Message& m = hist[mi];
        ImGui::PushID(mi);
        const bool fading = mi == g_fadeIdx && now - g_fadeStart < 0.3;
        if (fading) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (float)((now - g_fadeStart) / 0.3));
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        if (m.user) {
            // Your message sits in a tinted bubble.
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->ChannelsSplit(2);
            dl->ChannelsSetCurrent(1);
            const ImVec2 top = ImGui::GetCursorScreenPos();
            ImGui::Indent(6.0f);
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            ImGui::PushTextWrapPos(0.0f);
            DrawMarkdown(m.text, text);
            for (const ai::Attachment& a : m.attachments) {
                if (a.image) ImGui::TextDisabled("Image: %s (%dx%d)", a.name.c_str(), a.width, a.height);
                else ImGui::TextDisabled("File: %s", a.name.c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, 3.0f));
            ImGui::Unindent(6.0f);
            const ImVec2 bot = ImGui::GetCursorScreenPos();
            dl->ChannelsSetCurrent(0);
            dl->AddRectFilled(ImVec2(top.x, top.y), ImVec2(top.x + ImGui::GetContentRegionAvail().x, bot.y),
                              ImGui::GetColorU32(ImVec4(0.30f, 0.45f, 0.70f, 0.22f)), 6.0f);
            dl->ChannelsMerge();
        } else if (m.error) {
            if (m.stopped) DrawMarkdown("Stopped.", ImVec4(0.62f, 0.63f, 0.67f, 1.0f));
            else DrawMarkdown("**Error:** " + m.text, ImVec4(1.0f, 0.5f, 0.45f, 1.0f));
            if (mi == (int)hist.size() - 1 && !ai::Busy() && ImGui::SmallButton("Try again")) {
                ai::RetryLast(SceneContext());
                g_scrollToEnd = true;
            }
        } else {
            if (ImTextureID icon = AIProviderIcon(ai::ProviderIconIndex(ai::GetSettings().provider))) { ImGui::Image(icon, ImVec2(14.0f, 14.0f)); ImGui::SameLine(); }
            ImGui::TextDisabled("%s", ai::ProviderShortName(ai::GetSettings().provider));
            // A new reply is typed out (about 2 seconds for a long one) instead of appearing at once.
            std::string shown = m.text;
            bool revealing = false;
            if (mi == g_revealIdx) {
                const double perSecond = std::clamp((double)m.text.size() / 1.8, 200.0, 900.0);
                size_t n = (size_t)((now - g_revealStart) * perSecond);
                if (n < m.text.size()) {
                    revealing = true;
                    while (n > 0 && (m.text[n] & 0xC0) == 0x80) --n;      // never cut a UTF-8 character
                    shown = m.text.substr(0, n);
                    g_scrollToEnd = true;
                } else {
                    g_revealIdx = -1;
                }
            }
            const auto segs = ai::ParseReply(shown, revealing);
            int questionCount = 0;
            for (const ai::Segment& q : segs) questionCount += q.kind == ai::Segment::Question;
            const bool lastMessage = mi == (int)hist.size() - 1;
            const bool answerable = lastMessage && !revealing && !ai::Busy();
            int answered = 0;
            for (int si = 0; si < (int)segs.size(); ++si) {
                const ai::Segment& s = segs[si];
                ImGui::PushID(si);
                const auto key = std::make_pair(mi, si);
                if (s.kind == ai::Segment::Text) {
                    DrawMarkdown(s.text, text);
                } else if (s.kind == ai::Segment::Question) {
                    ImGui::Dummy(ImVec2(0.0f, 2.0f));
                    ImGui::TextColored(theme::AccentVec(), "Question");
                    DrawMarkdown(s.text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
                    auto found = g_answers.find(key);
                    int& chosen = g_answers[key];
                    if (found == g_answers.end()) chosen = -1;
                    for (int oi = 0; oi < (int)s.options.size(); ++oi) {
                        const std::string& opt = s.options[oi];
                        const float w = ImGui::GetContentRegionAvail().x;
                        const float th = ImGui::CalcTextSize(opt.c_str(), nullptr, false, w - 20.0f).y;
                        ImGui::PushID(oi);
                        const ImVec2 at = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##opt", ImVec2(w, th + 10.0f));
                        const bool hovered = ImGui::IsItemHovered() && answerable;
                        const bool picked = chosen == oi;
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        ImVec4 bg = picked ? theme::AccentVec(0.35f) : ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.14f : 0.07f);
                        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + th + 10.0f), ImGui::GetColorU32(bg), 5.0f);
                        if (picked) dl->AddRect(at, ImVec2(at.x + w, at.y + th + 10.0f), ImGui::GetColorU32(theme::AccentVec()), 5.0f);
                        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + 10.0f, at.y + 5.0f),
                                    ImGui::GetColorU32(ImVec4(1, 1, 1, answerable || picked ? 1.0f : 0.5f)), opt.c_str(), nullptr, w - 20.0f);
                        if (ImGui::IsItemClicked() && answerable) {
                            chosen = oi;
                            if (questionCount == 1) { ai::Send(opt, SceneContext()); g_scrollToEnd = true; }   // one question: the click answers it
                        }
                        ImGui::PopID();
                        ImGui::Dummy(ImVec2(0.0f, 2.0f));
                    }
                    answered += chosen >= 0;
                    if (answerable && questionCount == 1 && chosen < 0) ImGui::TextDisabled("or type your own answer below");
                } else {
                    const char* title = s.kind == ai::Segment::Console ? "Console commands"
                                      : s.kind == ai::Segment::Script ? s.name.c_str() : (s.name.empty() ? "code" : s.name.c_str());
                    ImGui::TextDisabled("%s", title);
                    ImGui::BeginChild("##code", ImVec2(0.0f, std::min(220.0f, ImGui::GetTextLineHeightWithSpacing() * (1.0f + std::count(s.text.begin(), s.text.end(), '\n')) + ImGui::GetStyle().WindowPadding.y * 2.0f + ImGui::GetStyle().ScrollbarSize)), true,
                                      ImGuiWindowFlags_HorizontalScrollbar);
                    ImGui::TextUnformatted(s.text.c_str());
                    ImGui::EndChild();
                    if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(s.text.c_str());
                    if (s.kind == ai::Segment::Script && IsPendingScript(s.name)) {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Discard")) {
                            const auto outcome = ai::RevertPendingScript(project::GetCurrentProject().path, s.name);
                            std::string out;
                            console::RunLine("rebuild", &out);
                            g_buildErrors.erase(key);
                            g_applied.erase(key);                  // it counts as a pending change again
                            for (auto it = g_watch.items.begin(); it != g_watch.items.end();) it = it->key == key ? g_watch.items.erase(it) : it + 1;
                            if (g_watch.items.empty()) g_watch.active = false;
                            g_applyResult[key] = outcome == ai::PendingOutcome::KeptEdited
                                ? s.name + " was edited since the AI wrote it, so it stays. Delete it yourself if you do not want it."
                                : "Discarded: " + s.name + " is back to how it was.";
                            RefreshPending(true);
                        }
                    }
                    auto it = g_applyResult.find(key);
                    if (it != g_applyResult.end()) {
                        if (ImTextureID icon = AIPanelIcon(3)) { ImGui::Image(icon, ImVec2(14.0f, 14.0f)); ImGui::SameLine(); }
                        ImGui::TextWrapped("%s", it->second.c_str());
                    }
                    if (g_buildErrors.count(key)) {
                        ImGui::BeginDisabled(ai::Busy());
                        if (ImGui::SmallButton("Send errors to AI")) {
                            ai::Send("The script " + s.name + " failed to compile with these errors:\n```\n" + g_buildErrors[key] +
                                     "```\nFix them and reply with the complete corrected file.", SceneContext());
                            g_buildErrors.erase(key);
                            g_scrollToEnd = true;
                        }
                        ImGui::EndDisabled();
                    }
                }
                ImGui::PopID();
            }
            if (questionCount > 1 && lastMessage && !revealing) {            // several questions: answer them all, then send together
                ImGui::BeginDisabled(!answerable || answered < questionCount);
                if (ImGui::Button("Send answers", ImVec2(-FLT_MIN, 0.0f))) {
                    std::string reply = "My answers:\n";
                    for (int si = 0; si < (int)segs.size(); ++si) {
                        if (segs[si].kind != ai::Segment::Question) continue;
                        const int c = g_answers[std::make_pair(mi, si)];
                        if (c >= 0 && c < (int)segs[si].options.size()) reply += "- " + segs[si].text + " " + segs[si].options[c] + "\n";
                    }
                    ai::Send(reply, SceneContext());
                    g_scrollToEnd = true;
                }
                ImGui::EndDisabled();
            }
        }
        if (fading) ImGui::PopStyleVar();
        ImGui::PopID();
    }
    if (ai::Busy()) {
        // The request is one call, so the phases are a time-based indication of activity, not real progress.
        if (!g_wasBusy) g_busyStart = now;
        const double t = now - g_busyStart;
        const int phase = t < 2.5 ? 0 : t < 6.0 ? 1 : 2;
        static const char* const labels[3] = { "Thinking", "Reading", "Writing" };
        const bool building = phase == 2 && ai::GetSettings().modeBuild;     // with Build on, the last stage is the building
        const int dots = (int)(now * 3.0) % 4;
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        PulsingIcon(AIPanelIcon(building ? 5 : phase), 18.0f, now);
        ImGui::SameLine();
        ImGui::TextColored(theme::AccentVec(), "%s%.*s", building ? "Building" : labels[phase], dots, ".");
        g_scrollToEnd = true;
    }
    if (g_watch.active) {                                                    // scripts that were just written are compiling
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        PulsingIcon(AIPanelIcon(5), 18.0f, now);
        ImGui::SameLine();
        ImGui::TextColored(theme::AccentVec(), "Building%.*s", (int)(now * 3.0) % 4, ".");
        g_scrollToEnd = true;
    }
    g_wasBusy = ai::Busy();
    if (hist.size() != g_lastCount) { g_lastCount = hist.size(); g_scrollToEnd = true; }
    if (g_scrollToEnd) { ImGui::SetScrollHereY(1.0f); g_scrollToEnd = false; }
    ImGui::EndChild();

    if (showBar) DrawChangesBar(changes, flashing);

    // Like the Claude app: a + for what you want to do, the message box, with the model picker and Send underneath.
    ai::Settings& modes = ai::GetSettings();
    if (modes.modeScript || modes.modeBuild) {                             // the chosen modes, each removable
        auto chip = [&](const char* label, bool& flag) {
            if (!flag) return;
            ImGui::PushStyleColor(ImGuiCol_Button, theme::AccentVec(0.30f));
            if (ImGui::SmallButton((std::string(label) + "  x").c_str())) { flag = false; ai::Save(); }
            ImGui::PopStyleColor();
            ImGui::SameLine();
        };
        chip("Script", modes.modeScript);
        chip("Build", modes.modeBuild);
        ImGui::NewLine();
    }
    for (size_t ai_i = 0; ai_i < g_attach.size(); ++ai_i) {                // what will be sent along, each removable
        const ai::Attachment& a = g_attach[ai_i];
        const float rowH = std::max(ImGui::GetFrameHeight(), 26.0f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, ImVec2(at.x + w, at.y + rowH), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.07f)), 5.0f);
        float x = at.x + 4.0f;
        if (g_attachThumb[ai_i].id != 0) {
            const float th = rowH - 6.0f, tw = th * (float)a.thumbW / (float)std::max(1, a.thumbH);
            dl->AddImage(static_cast<ImTextureID>(GetTextureImGuiId(g_attachThumb[ai_i])), ImVec2(x, at.y + 3.0f), ImVec2(x + tw, at.y + 3.0f + th));
            x += tw + 6.0f;
        }
        char label[160];
        if (a.image) std::snprintf(label, sizeof label, "%s  %dx%d", a.name.c_str(), a.width, a.height);
        else std::snprintf(label, sizeof label, "%s  %zu KB", a.name.c_str(), std::max<size_t>(1, a.data.size() / 1024));
        dl->PushClipRect(ImVec2(x, at.y), ImVec2(at.x + w - rowH, at.y + rowH), true);
        dl->AddText(ImVec2(x, at.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(ImVec4(0.9f, 0.9f, 0.92f, 1.0f)), label);
        dl->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(at.x + w - rowH + 2.0f, at.y + 2.0f));
        ImGui::PushID((int)ai_i);
        const bool remove = ImGui::SmallButton("x");
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH));
        ImGui::Dummy(ImVec2(0.0f, style.ItemSpacing.y));
        if (remove) { RemoveAttachment(ai_i); break; }
    }
    if (attachError) ImGui::PushTextWrapPos(0.0f), ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", g_attachError.c_str()), ImGui::PopTextWrapPos();
    bool send = false;
    const float plusW = ImGui::GetFrameHeight();
    const float rowY = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(rowY + boxH - plusW);                              // the + sits at the bottom-left of the box
    if (ImGui::Button("+##aiplus", ImVec2(plusW, plusW))) ImGui::OpenPopup("##aimodes");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("What do you want to do?");
    ImGui::SameLine();
    ImGui::SetCursorPosY(rowY);
    const ImVec2 boxPos = ImGui::GetCursorScreenPos();
    send |= ImGui::InputTextMultiline("##aiinput", g_input, sizeof g_input, ImVec2(-FLT_MIN, boxH),
                                       ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_WordWrap);
    if (!g_input[0] && !ImGui::IsItemActive())
        ImGui::GetWindowDrawList()->AddText(ImVec2(boxPos.x + style.FramePadding.x, boxPos.y + style.FramePadding.y), IM_COL32(150, 150, 150, 255), "Ask Flyengine AI...");
    if (ImGui::BeginPopup("##aimodes")) {
        ImGui::TextDisabled("Add to your message");
        ImGui::BeginDisabled(platform::DialogPending() || (int)g_attach.size() >= ai::kMaxAttachments);
        if (ImGui::MenuItem("Attach file...", "text or code")) {
            g_attachAsImage = false;
            platform::BeginOpenFileDialog(platform::DialogPurpose::AIAttach, "Attach a file", project::GetCurrentProject().path, {});
        }
        if (ImGui::MenuItem("Attach image...", "png, jpg, ...")) {
            g_attachAsImage = true;
            platform::BeginOpenFileDialog(platform::DialogPurpose::AIAttach, "Attach an image", project::GetCurrentProject().path,
                                          { { "Images", "*.png *.jpg *.jpeg *.bmp *.gif *.tga *.psd" } });
        }
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::TextDisabled("What do you want to do?");
        bool changed = false;
        changed |= ImGui::MenuItem("Script", "write scripts", &modes.modeScript);
        changed |= ImGui::MenuItem("Build", "make things in the game", &modes.modeBuild);
        ImGui::Separator();
        changed |= ImGui::MenuItem("Ask me questions", "instead of guessing", &modes.modeAsk);
        if (!modes.modeScript && !modes.modeBuild) { ImGui::Separator(); ImGui::TextDisabled("Both off: explanations only."); }
        if (changed) ai::Save();
        ImGui::EndPopup();
    }

    const float sendW = 56.0f, gap = ImGui::GetStyle().ItemSpacing.x;
    DrawAIModelCombo("##aimodelpick", std::max(70.0f, ImGui::GetContentRegionAvail().x - sendW - gap));
    ImGui::SameLine();
    if (ai::Busy() || g_revealIdx >= 0) {
        // Stop abandons the request (the transfer is aborted); while a reply is being typed out it shows the rest at once.
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.26f, 0.26f, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.32f, 0.32f, 1.0f));
        if (ImGui::Button("Stop", ImVec2(sendW, 0.0f))) {
            if (ai::Busy()) ai::Stop();
            g_revealIdx = -1;
            g_scrollToEnd = true;
        }
        ImGui::PopStyleColor(2);
    } else {
        send |= ImGui::Button("Send", ImVec2(sendW, 0.0f));
    }
    if (send && !ai::Busy() && (g_input[0] || !g_attach.empty())) {
        std::vector<ai::Attachment> sending = std::move(g_attach);
        g_attach.clear();
        for (Texture2D& t : g_attachThumb) if (t.id != 0) UnloadTexture(t);
        g_attachThumb.clear();
        ai::Send(g_input, SceneContext(), std::move(sending));
        g_input[0] = 0;
        g_scrollToEnd = true;
    }
    ImGui::End();
}

} // namespace ui
