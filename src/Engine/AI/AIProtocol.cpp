// AIProtocol.cpp: parsing the model's reply into text / Apply-able blocks, and the safety checks on what Apply may do.
#include "../../../include/Engine/AI/AI.hpp"
#include <cctype>
#include <sstream>

namespace ai {

bool IsSafeScriptName(const std::string& n) {
    if (n.size() < 5 || n.size() > 64) return false;
    if (n.compare(n.size() - 4, 4, ".cpp") != 0) return false;
    const std::string stem = n.substr(0, n.size() - 4);
    if (!(std::isalpha((unsigned char)stem[0]) || stem[0] == '_')) return false;
    for (char c : stem) if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

bool IsAllowedConsoleLine(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
    // FlyCloud commands upload projects and change account keys: never run those from a model's reply.
    return line.compare(i, 6, "fcloud") != 0;
}

std::string DeriveScriptName(const std::string& code) {
    const size_t at = code.find("FLY_SCRIPT(");
    if (at == std::string::npos) return {};
    size_t i = at + 11;
    while (i < code.size() && code[i] == ' ') ++i;
    std::string cls;
    while (i < code.size() && (std::isalnum((unsigned char)code[i]) || code[i] == '_')) cls += code[i++];
    return cls.empty() ? std::string() : cls + ".cpp";
}

// Models write the file name in many ways ("Spinner.cpp", "Scripts/Spinner.cpp", "Spinner", "`Spinner.cpp`"). Only the base
// name is kept, so a path can never leave the Scripts folder; with no usable name the FLY_SCRIPT class name is used.
std::string NormalizeScriptName(const std::string& raw, const std::string& code) {
    std::string n;
    for (char c : raw) if (c != '`' && c != '"' && c != '\'' && c != ' ' && c != '\t' && c != '\r') n += c;
    const size_t slash = n.find_last_of("/\\");
    if (slash != std::string::npos) n = n.substr(slash + 1);
    if (n.empty()) return DeriveScriptName(code);
    if (n.find('.') == std::string::npos) n += ".cpp";
    return n;
}

std::vector<Segment> ParseReply(const std::string& reply, bool partial) {
    std::vector<Segment> out;
    auto pushText = [&](const std::string& t) {
        if (t.find_first_not_of(" \t\r\n") == std::string::npos) return;
        Segment s; s.kind = Segment::Text; s.text = t; out.push_back(std::move(s));
    };
    size_t pos = 0;
    while (pos < reply.size()) {
        const size_t open = reply.find("```", pos);
        if (open == std::string::npos) { pushText(reply.substr(pos)); break; }
        // The fence must start a line.
        if (open != 0 && reply[open - 1] != '\n') { pushText(reply.substr(pos, open + 3 - pos)); pos = open + 3; continue; }
        const size_t infoEnd = reply.find('\n', open + 3);
        if (infoEnd == std::string::npos) { pushText(reply.substr(pos)); break; }
        const size_t close = reply.find("\n```", infoEnd);
        if (close == std::string::npos) {
            if (partial) {                              // still arriving: show what there is as code, never as an Apply block
                pushText(reply.substr(pos, open - pos));
                Segment c; c.kind = Segment::Code; c.text = reply.substr(infoEnd + 1);
                out.push_back(std::move(c));
            } else {
                pushText(reply.substr(pos));            // unterminated: show as text
            }
            break;
        }
        pushText(reply.substr(pos, open - pos));
        std::string info = reply.substr(open + 3, infoEnd - open - 3);
        while (!info.empty() && (info.back() == '\r' || info.back() == ' ')) info.pop_back();
        Segment s;
        s.text = reply.substr(infoEnd + 1, close - infoEnd - 1);
        const bool cppInfo = info == "cpp" || info == "c++" || info == "cxx" || info.empty();
        if (info == "fly-question") {
            s.kind = Segment::Question;
            std::istringstream lines(s.text);
            std::string first, line;
            s.text.clear();
            while (std::getline(lines, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                size_t i = 0;
                while (i < line.size() && line[i] == ' ') ++i;
                line.erase(0, i);
                if (line.empty()) continue;
                const bool bullet = line.size() > 2 && (line[0] == '-' || line[0] == '*') && line[1] == ' ';
                size_t d = 0;
                while (d < line.size() && std::isdigit((unsigned char)line[d])) ++d;
                const bool numbered = d > 0 && d + 1 < line.size() && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ';
                if (bullet || numbered) {
                    std::string opt = line.substr(bullet ? 2 : d + 2), plain;   // an option is a button label: no markdown marks
                    for (size_t k = 0; k < opt.size(); ++k) {
                        if (opt[k] == '`') continue;
                        if (opt[k] == '*' && k + 1 < opt.size() && opt[k + 1] == '*') { ++k; continue; }
                        plain += opt[k];
                    }
                    if (s.options.size() < 6) s.options.push_back(plain);
                } else if (s.options.empty()) {
                    s.text += (s.text.empty() ? "" : " ") + line;     // the question may wrap over lines
                }
            }
        } else if (info == "fly-console") s.kind = Segment::Console;
        else if (info.rfind("fly-script", 0) == 0) {
            s.kind = Segment::Script;
            std::string rest = info.substr(10);
            if (!rest.empty() && (rest[0] == ':' || rest[0] == ' ')) rest.erase(0, 1);
            s.name = NormalizeScriptName(rest, s.text);
        } else if (cppInfo && s.text.find("FLY_SCRIPT(") != std::string::npos) {
            s.kind = Segment::Script;                  // a plain ```cpp block that is a Flyengine script still gets Apply
            s.name = DeriveScriptName(s.text);
        } else s.kind = Segment::Code, s.name = info;
        out.push_back(std::move(s));
        pos = close + 4;
        if (pos < reply.size() && reply[pos] == '\r') ++pos;
        if (pos < reply.size() && reply[pos] == '\n') ++pos;
    }
    return out;
}

} // namespace ai

namespace ai {

std::string ModeInstructions(bool script, bool build, bool ask) {
    std::string t = "Modes the user chose with the + button next to the message box:\n";
    if (!script && !build)
        t += "- Script and Build are both OFF: this is an explanation-only conversation. Do NOT write fly-script or fly-console blocks (they "
             "cannot be applied). If the user asks you to make or change something, tell them to turn on Script (C++ gameplay scripts) or "
             "Build (create and change things in the scene) with the + button, and explain what you would do.\n";
    else {
        t += script ? "- Script is ON: you may write fly-script blocks.\n" : "- Script is OFF: do not write fly-script blocks.\n";
        t += build ? "- Build is ON: you may use fly-console blocks to create and change things in the scene.\n"
                   : "- Build is OFF: do not write fly-console blocks.\n";
    }
    t += ask ? "- Questions are ON: when a request is ambiguous or you need a choice, ask with a fly-question block instead of guessing.\n"
             : "- Questions are OFF: do not ask questions; pick sensible defaults and say what you assumed.\n";
    return t;
}

} // namespace ai
