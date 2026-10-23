// ChatStore.cpp: chats saved on this machine, one JSON file each under <ConfigDir>/chats. Never inside a project, so they
// are not exported or committed; the files are owner-only because a chat can quote project code.
#include "../../../include/Engine/AI/AI.hpp"

#if FLYENGINE_ENABLE_FLYCLOUD

#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/U8Path.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ai {

namespace {
namespace fs = std::filesystem;
std::string g_dirOverride;

std::string ChatsDir() {
    if (!g_dirOverride.empty()) return g_dirOverride;
    const std::string dir = platform::ConfigDir();
    return dir.empty() ? std::string() : dir + "/chats";
}

fs::path FileFor(const std::string& id) { return U8Path(ChatsDir()) / (id + ".json"); }

void OwnerOnly(const fs::path& p, fs::perms perms) {
    std::error_code ec;
    fs::permissions(p, perms, fs::perm_options::replace, ec);
}
}

void SetChatsDirForTesting(const std::string& dir) { g_dirOverride = dir; }

bool IsValidChatId(const std::string& id) {
    if (id.empty() || id.size() > 48) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

bool SaveChatFile(const ChatInfo& meta, const std::vector<Message>& msgs) {
    if (platform::IsEmbedded() || !IsValidChatId(meta.id) || ChatsDir().empty()) return false;
    using nlohmann::json;
    json j;
    j["id"] = meta.id;
    j["title"] = meta.title;
    j["updated"] = meta.updated;
    json arr = json::array();
    for (const Message& m : msgs) {
        if (m.error) continue;                   // error notices are not part of the conversation
        json entry = { { "role", m.user ? "user" : "assistant" }, { "text", m.text } };
        if (!m.attachments.empty()) {                    // files keep their text; an image keeps only its name and size (the picture is not stored)
            json files = json::array();
            for (const Attachment& a : m.attachments) {
                json f = { { "name", a.name }, { "image", a.image } };
                if (a.image) { f["width"] = a.width; f["height"] = a.height; }
                else f["text"] = a.data;
                files.push_back(f);
            }
            entry["attachments"] = files;
        }
        arr.push_back(entry);
    }
    j["messages"] = arr;

    std::error_code ec;
    fs::create_directories(U8Path(ChatsDir()), ec);
    OwnerOnly(U8Path(ChatsDir()), fs::perms::owner_all);
    const fs::path file = FileFor(meta.id);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream touch(tmp, std::ios::app);
    }
    OwnerOnly(tmp, fs::perms::owner_read | fs::perms::owner_write);
    {
        std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
        if (!out) return false;
        out << j.dump(1, ' ', false, json::error_handler_t::replace);
        if (!out) return false;
    }
    fs::rename(tmp, file, ec);                   // a crash mid-write never leaves a half chat
    return !ec;
}

bool LoadChatFile(const std::string& id, ChatInfo& meta, std::vector<Message>& msgs) {
    if (!IsValidChatId(id)) return false;
    using nlohmann::json;
    std::ifstream in(FileFor(id), std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    const json j = json::parse(ss.str(), nullptr, false);
    if (!j.is_object() || !j.contains("messages") || !j["messages"].is_array()) return false;
    try {
    meta.id = id;
    meta.title = j.value("title", std::string("Chat"));
    meta.updated = j.value("updated", 0LL);
    msgs.clear();
    for (const auto& m : j["messages"]) {
        if (!m.is_object() || !m.contains("text") || !m["text"].is_string()) continue;
        Message out;
        out.user = m.value("role", "user") == "user";
        out.text = m["text"].get<std::string>();
        if (m.contains("attachments") && m["attachments"].is_array()) {
            for (const auto& f : m["attachments"]) {
                if (!f.is_object()) continue;
                Attachment a;
                a.name = f.value("name", std::string("file"));
                a.image = f.value("image", false);
                a.width = f.value("width", 0);
                a.height = f.value("height", 0);
                if (!a.image && f.contains("text") && f["text"].is_string()) a.data = f["text"].get<std::string>();
                out.attachments.push_back(std::move(a));
            }
        }
        msgs.push_back(std::move(out));
    }
    } catch (const json::exception&) { msgs.clear(); return false; }   // wrong-typed field in a damaged or hand-edited file
    return true;
}

std::vector<ChatInfo> ListChats() {
    std::vector<ChatInfo> out;
    if (platform::IsEmbedded() || ChatsDir().empty()) return out;
    std::error_code ec;
    fs::directory_iterator it(U8Path(ChatsDir()), ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        ChatInfo info;
        std::vector<Message> unused;
        if (LoadChatFile(it->path().stem().string(), info, unused)) out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const ChatInfo& a, const ChatInfo& b) { return a.updated > b.updated; });
    return out;
}

void DeleteChatFile(const std::string& id) {
    if (!IsValidChatId(id)) return;
    std::error_code ec;
    fs::remove(FileFor(id), ec);
}

} // namespace ai

#else

namespace ai {
void SetChatsDirForTesting(const std::string&) {}
bool IsValidChatId(const std::string&) { return false; }
bool SaveChatFile(const ChatInfo&, const std::vector<Message>&) { return false; }
bool LoadChatFile(const std::string&, ChatInfo&, std::vector<Message>&) { return false; }
std::vector<ChatInfo> ListChats() { return {}; }
void DeleteChatFile(const std::string&) {}
}

#endif
