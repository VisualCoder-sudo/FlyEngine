// PendingChanges.cpp: scripts the AI wrote stay "unsaved" until the project is saved, the same rule as imports and new script
// stubs. State lives on disk in <project>/.ai_pending so a crash cannot leave them behind:
//   journal.txt        one line per file: "<Name.cpp>\t<new|replaced>"
//   <Name.cpp>.orig    the file as it was before the AI overwrote it ("replaced" only)
//   <Name.cpp>.ai      exactly what the AI wrote, to tell later hand edits from the AI's text
#include "../../../include/Engine/AI/AI.hpp"
#include "../../../include/Engine/U8Path.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

namespace ai {

namespace {
namespace fs = std::filesystem;

struct Entry { std::string name; bool replaced = false; };

fs::path PendingDir(const std::string& project) { return U8Path(project) / ".ai_pending"; }
fs::path ScriptsDir(const std::string& project) { return U8Path(project) / "Scripts"; }

bool ReadAll(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool WriteAll(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), (std::streamsize)data.size());
    return static_cast<bool>(out);
}

std::vector<Entry> ReadJournal(const std::string& project) {
    std::vector<Entry> out;
    if (project.empty()) return out;                 // never resolve ".ai_pending" against the working directory
    std::ifstream in(PendingDir(project) / "journal.txt");
    for (std::string line; std::getline(in, line);) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        Entry e;
        e.name = line.substr(0, tab);
        e.replaced = line.substr(tab + 1) == "replaced";
        if (IsSafeScriptName(e.name)) out.push_back(std::move(e));   // a hand-edited journal cannot point outside Scripts/
    }
    return out;
}

void WriteJournal(const std::string& project, const std::vector<Entry>& entries) {
    std::error_code ec;
    if (entries.empty()) { fs::remove_all(PendingDir(project), ec); return; }
    fs::create_directories(PendingDir(project), ec);
    std::ofstream out(PendingDir(project) / "journal.txt", std::ios::trunc);
    for (const Entry& e : entries) out << e.name << '\t' << (e.replaced ? "replaced" : "new") << '\n';
}

void DropSideFiles(const std::string& project, const std::string& name) {
    std::error_code ec;
    fs::remove(PendingDir(project) / (name + ".orig"), ec);
    fs::remove(PendingDir(project) / (name + ".ai"), ec);
}
}

bool WritePendingScript(const std::string& project, const std::string& name, const std::string& code, std::string& error) {
    if (project.empty()) { error = "No project is open."; return false; }
    if (!IsSafeScriptName(name)) { error = "'" + name + "' is not a plain Name.cpp file name."; return false; }
    std::error_code ec;
    fs::create_directories(ScriptsDir(project), ec);
    fs::create_directories(PendingDir(project), ec);
    const fs::path file = ScriptsDir(project) / name;

    std::vector<Entry> entries = ReadJournal(project);
    bool known = false;
    for (const Entry& e : entries) known |= e.name == name;
    if (!known) {                                   // first AI write to this file: remember what was there
        Entry e;
        e.name = name;
        std::string before;
        if (fs::exists(file, ec)) {
            if (!ReadAll(file, before)) { error = "Could not read the existing " + name + " to back it up."; return false; }
            if (!WriteAll(PendingDir(project) / (name + ".orig"), before)) { error = "Could not back up the existing " + name + "."; return false; }
            e.replaced = true;
        }
        entries.push_back(e);
        WriteJournal(project, entries);             // journal first: a crash after the write below still knows about the file
    }
    if (!WriteAll(file, code + "\n")) { error = "Could not write " + file.string(); return false; }
    WriteAll(PendingDir(project) / (name + ".ai"), code + "\n");
    return true;
}

bool IsScriptPending(const std::string& project, const std::string& name) {
    for (const Entry& e : ReadJournal(project)) if (e.name == name) return true;
    return false;
}

std::vector<std::string> PendingScripts(const std::string& project) {
    std::vector<std::string> out;
    for (const Entry& e : ReadJournal(project)) out.push_back(e.name);
    return out;
}

PendingOutcome RevertPendingScript(const std::string& project, const std::string& name) {
    std::vector<Entry> entries = ReadJournal(project);
    auto it = entries.begin();
    while (it != entries.end() && it->name != name) ++it;
    if (it == entries.end()) return PendingOutcome::NotPending;

    const fs::path file = ScriptsDir(project) / name;
    std::string current, written;
    const bool haveFile = ReadAll(file, current);
    const bool haveAi = ReadAll(PendingDir(project) / (name + ".ai"), written);
    PendingOutcome outcome = PendingOutcome::Reverted;
    std::error_code ec;
    if (haveFile && (!haveAi || current != written)) {
        outcome = PendingOutcome::KeptEdited;       // edited since (or no record of what the AI wrote): the user's work wins
    } else if (it->replaced) {
        std::string original;
        if (!ReadAll(PendingDir(project) / (name + ".orig"), original) || !WriteAll(file, original))
            return PendingOutcome::KeptEdited;       // could not restore: keep the journal and backup so it can be retried
    } else {
        fs::remove(file, ec);
    }
    DropSideFiles(project, name);
    entries.erase(it);
    WriteJournal(project, entries);
    return outcome;
}

int RevertAllPendingScripts(const std::string& project) {
    int reverted = 0;
    for (const std::string& name : PendingScripts(project))
        reverted += RevertPendingScript(project, name) == PendingOutcome::Reverted;
    return reverted;
}

void CommitPendingScripts(const std::string& project) {
    if (project.empty()) return;
    std::error_code ec;
    fs::remove_all(PendingDir(project), ec);
}

} // namespace ai
