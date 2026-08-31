#include "Flyscript.hpp"
#include "PhysicsSimulation.hpp"
#include "Graphics.hpp"
#include "Engine.hpp"
#include "ui.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unordered_map>

namespace flyscript {

namespace {

bool ParseBool(const std::string& value, bool& out) {
    if (value == "true" || value == "1") { out = true; return true; }
    if (value == "false" || value == "0") { out = false; return true; }
    return false;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool ParseVector3(const std::string& value, Vector3& out) {
    std::string t = Trim(value);
    size_t open = t.find('(');
    size_t close = t.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    if (!Trim(t.substr(close + 1)).empty()) return false;

    std::string prefix = Trim(t.substr(0, open));
    if (!prefix.empty()) {
        std::string lower = prefix;
        for (char& ch : lower) ch = (char)tolower((unsigned char)ch);
        if (lower != "vector3.new") return false;
    }

    std::string inner = t.substr(open + 1, close - open - 1);
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= inner.size(); ++i) {
        if (i == inner.size() || inner[i] == ',') {
            parts.push_back(Trim(inner.substr(start, i - start)));
            start = i + 1;
        }
    }
    if (parts.size() != 3) return false;
    out = { (float)atof(parts[0].c_str()), (float)atof(parts[1].c_str()), (float)atof(parts[2].c_str()) };
    return true;
}

bool ParseNumberList(const std::string& value, std::vector<float>& out) {
    std::string t = Trim(value);
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == ',') {
            std::string part = Trim(t.substr(start, i - start));
            if (part.empty()) return false;
            out.push_back((float)atof(part.c_str()));
            start = i + 1;
        }
    }
    return true;
}

unsigned char ClampU8(float f) {
    int i = (int)f;
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return (unsigned char)i;
}

Color MakeRgbColor(float r, float g, float b, bool hasMods, float sat, float bright) {
    Color c = { ClampU8(r), ClampU8(g), ClampU8(b), 255 };
    if (hasMods) {
        Vector3 hsv = ColorToHSV(c);
        float s = sat / 255.0f;   if (s < 0.0f) s = 0.0f; if (s > 1.0f) s = 1.0f;
        float v = bright / 255.0f; if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
        c = ColorFromHSV(hsv.x, s, v);
    }
    return c;
}

bool ParseColor(const std::string& value, Color& out) {
    std::string t = Trim(value);
    if (t.size() >= 5 && t.front() == '(' && t.back() == ')') {
        std::vector<float> nums;
        if (!ParseNumberList(t.substr(1, t.size() - 2), nums)) return false;
        if (nums.size() != 3) return false;
        out = MakeRgbColor(nums[0], nums[1], nums[2], false, 0.0f, 0.0f);
        return true;
    }

    size_t open = t.find('(');
    size_t close = t.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) return false;
    std::string name = Trim(t.substr(0, open));
    if (name.size() != 3) return false;
    if (tolower((unsigned char)name[0]) != 'r' ||
        tolower((unsigned char)name[1]) != 'g' ||
        tolower((unsigned char)name[2]) != 'b') return false;

    std::vector<float> nums;
    if (!ParseNumberList(t.substr(open + 1, close - open - 1), nums)) return false;
    if (nums.size() == 3) { out = MakeRgbColor(nums[0], nums[1], nums[2], false, 0.0f, 0.0f); return true; }
    if (nums.size() == 5) { out = MakeRgbColor(nums[0], nums[1], nums[2], true, nums[3], nums[4]); return true; }
    return false;
}

bool SetObjectProperty(ScatteredObject* target, const std::string& prop,
                       const std::string& rhs, Runtime& rt, std::string& outError) {
    rt.LogPropertyChange(target, prop, rhs);
    if (prop == "Position") {
        Vector3 v;
        if (!ParseVector3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (rt.IsSimPlaying()) rt.SetBodyPosition(target, v);
        *target->GetPosPtr() = v;
        return true;
    }
    if (prop == "Size") {
        Vector3 v;
        if (!ParseVector3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        *target->GetSizePtr() = v;
        target->ResetMassAuto();
        return true;
    }
    if (prop == "Mass") {
        float m = (float)atof(rhs.c_str());
        if (!(m > 0.0f)) { outError = "'" + rhs + "' is not a valid positive mass"; return false; }
        target->SetMass(m);
        return true;
    }
    if (prop == "Rotation") {
        Vector3 v;
        if (!ParseVector3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (rt.IsSimPlaying()) rt.SetBodyOrientation(target, v);
        *target->GetRotationPtr() = v;
        return true;
    }
    if (prop == "Origin") {
        Vector3 v;
        if (!ParseVector3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        *target->GetOriginPtr() = v;
        return true;
    }
    if (prop == "Color") {
        Color c;
        if (!ParseColor(rhs, c)) { outError = "'" + rhs + "' is not a valid color ((r, g, b) or rgb(r, g, b[, sat, bright]))"; return false; }
        *target->GetColorPtr() = c;
        return true;
    }
    if (prop == "Velocity") {
        Vector3 v;
        if (!ParseVector3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (rt.IsSimPlaying()) rt.SetBodyVelocity(target, v);
        target->SetVelocity(v);
        return true;
    }
    if (prop == "AngularVelocity") {
        Vector3 v;
        if (!ParseVector3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (rt.IsSimPlaying()) rt.SetBodyAngularVelocity(target, v);
        target->SetAngularVelocity(v);
        return true;
    }
    if (prop == "Anchored") {
        bool value = false;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        target->anchored = value;
        return true;
    }
    if (prop == "CanCollide") {
        bool value = false;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        target->canCollide = value;
        return true;
    }
    if (prop == "CollisionAccuracy") {
        pcoll::CollisionAccuracy acc;
        if (!pcoll::ParseCollisionAccuracy(rhs, acc)) {
            outError = "'" + rhs + "' is not a valid collision accuracy (Box, Hull, Default, Precise)";
            return false;
        }
        target->SetCollisionAccuracy(acc);
        return true;
    }
    if (prop == "Transparency") {
        float t = (float)atof(rhs.c_str());
        if (t < 0.0f || t > 1.0f) { outError = "'" + rhs + "' is out of range (0 to 1)"; return false; }
        target->SetTransparency(t);
        return true;
    }
    outError = "Unknown property '" + prop + "'";
    return false;
}

}

ModelGroup* FindModelByName(Runtime& rt, const std::string& name) {
    for (const auto& m : rt.GetModels())
        if (m && m->name == name) return m.get();
    return nullptr;
}

static bool ResolveWorkspaceObject(const std::string& rest, Runtime& rt,
                                   ScatteredObject*& out, std::string &err) {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start <= rest.size()) {
        size_t dot = rest.find('.', start);
        segs.push_back(rest.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    if (segs.empty() || segs[0].empty()) {
        err = "Invalid object path 'game.Workspace." + rest + "'";
        return false;
    }

    ModelGroup* model = FindModelByName(rt, segs[0]);
    if (model) {
        if (segs.size() == 1) {
            std::string parts;
            int shown = 0;
            for (auto* m : model->members) {
                if (!m) continue;
                if (!parts.empty()) parts += ", ";
                parts += m->GetName();
                if (++shown >= 8) { parts += ", ..."; break; }
            }
            err = "'" + segs[0] + "' is a model. Reference a part, e.g. game.Workspace." +
                  segs[0] + ".PartName";
            if (!parts.empty()) err += " (parts: " + parts + ")";
            return false;
        }
        for (auto* m : model->members) {
            if (m && m->GetName() == segs[1]) { out = m; return true; }
        }
        err = "Cannot find part '" + segs[1] + "' in model '" + segs[0] + "'";
        return false;
    }

    ScatteredObject* obj = rt.FindByName(segs[0]);
    if (!obj) {
        std::string avail;
        std::vector<std::string> seen;
        int shown = 0;
        for (auto* o : rt.GetObjects()) {
            if (!o) continue;
            std::string n = o->GetName();
            if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
            if (!avail.empty()) avail += ", ";
            avail += n;
            seen.push_back(n);
            if (++shown >= 6) break;
        }
        err = "Cannot find object '" + segs[0] + "' in the workspace";
        if (!avail.empty()) err += " (objects: " + avail + (shown >= 6 ? ", ..." : "") + ")";
        return false;
    }
    if (segs.size() > 1) {
        err = "Object '" + segs[0] + "' has no member '" + segs[1] + "'";
        return false;
    }
    out = obj;
    return true;
}

bool ExecuteAssignment(const std::string& path, const std::string& property,
                       const std::string& rhs, ScatteredObject* self,
                       Runtime& rt, std::string& outError) {

    if (path == "game.Lighting.GlobalShadows") {
        bool value = false;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        gfx::SetShadowsEnabled(value);
        return true;
    }
    if (path == "game.Lighting.Ambient") {
        float intensity = (float)atof(rhs.c_str());
        if (intensity < 0.0f || intensity > 2.0f) { outError = "'" + rhs + "' is out of range (0.0 to 2.0)"; return false; }
        gfx::SetAmbientIntensity(intensity);
        return true;
    }
    if (path == "game.Lighting.ShadowQuality") {
        int quality = atoi(rhs.c_str());
        if (quality < 5 || quality > 100) { outError = "'" + rhs + "' is out of range (5 to 100)"; return false; }
        gfx::SetShadowQuality(quality);
        return true;
    }
    if (path == "game.Rendering.Grid") {
        bool value = false;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        gfx::SetGridVisible(value);
        return true;
    }
    if (path == "game.Rendering.Wireframe") {
        bool value = false;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        gfx::SetWireframe(value);
        return true;
    }
    if (path == "game.Camera.FOV") {
        float fov = (float)atof(rhs.c_str());
        if (fov < 10.0f || fov > 170.0f) { outError = "'" + rhs + "' is out of range (10 to 170)"; return false; }
        rt.GetCamera().fovy = fov;
        return true;
    }
    if (path == "game.Physics.Gravity") {
        float g = (float)atof(rhs.c_str());
        if (g < -30.0f || g > 0.0f) { outError = "'" + rhs + "' is out of range (-30 to 0)"; return false; }
        rt.SetPhysicsGravity(g);
        return true;
    }
    if (path == "game.Physics.Friction") {
        float f = (float)atof(rhs.c_str());
        if (f < 0.0f || f > 1.0f) { outError = "'" + rhs + "' is out of range (0 to 1)"; return false; }
        rt.SetPhysicsFriction(f);
        return true;
    }
    if (path == "game.Physics.Restitution") {
        float r = (float)atof(rhs.c_str());
        if (r < 0.0f || r > 1.0f) { outError = "'" + rhs + "' is out of range (0 to 1)"; return false; }
        rt.SetPhysicsRestitution(r);
        return true;
    }

    ScatteredObject* target = nullptr;
    std::string prop;
    if (path.rfind("self.", 0) == 0) {
        if (!self) { outError = "No 'self' object in this script context"; return false; }
        target = self;
        prop = path.substr(5);
    } else if (path.rfind("game.Workspace.", 0) == 0) {
        std::string rest = path.substr(15);
        size_t dot = rest.rfind('.');
        std::string objPath;
        if (dot != std::string::npos) {
            objPath = rest.substr(0, dot);
            prop = rest.substr(dot + 1);
        } else {
            objPath = rest;
            prop = property;
        }
        if (!ResolveWorkspaceObject(objPath, rt, target, outError)) return false;
    }
    if (target) {
        return SetObjectProperty(target, prop, rhs, rt, outError);
    }

    outError = "Unknown property '" + path + "." + property + "'";
    return false;
}

namespace {

struct KnownProp {
    const char* path;
    bool isBool;
};

const KnownProp kKnownProps[] = {
    { "game.Lighting.GlobalShadows", true },
    { "game.Lighting.ShadowQuality", false },
    { "game.Lighting.Ambient", false },
    { "game.Rendering.Grid", true },
    { "game.Rendering.Wireframe", true },
    { "game.Camera.FOV", false },
    { "game.Physics.Gravity", false },
    { "game.Physics.Friction", false },
    { "game.Physics.Restitution", false },
};

struct ObjectProp {
    const char* name;
    bool isBool;
};

const ObjectProp kObjectProps[] = {
    { "Position", false },
    { "Size", false },
    { "Rotation", false },
    { "Origin", false },
    { "Color", false },
    { "Velocity", false },
    { "AngularVelocity", false },
    { "Anchored", true },
    { "CanCollide", true },
    { "CollisionAccuracy", false },
    { "Mass", false },
    { "Transparency", false },
};

bool CIStartsWith(const std::string& text, const std::string& prefix) {
    if (text.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (tolower((unsigned char)text[i]) != tolower((unsigned char)prefix[i])) return false;
    return true;
}

void CollectFullPaths(std::vector<std::string>& fulls) {
    for (const auto& kp : kKnownProps) fulls.push_back(std::string(kp.path) + ".value");
    if (IsRuntimeReady()) {
        for (auto *obj: GetRuntime().GetObjects()) {
            if (!obj) continue;
            for (const auto& op : kObjectProps)
                fulls.push_back("game.Workspace." + obj->GetName() + "." + op.name + ".value");
        }
        for (const auto& m : GetRuntime().GetModels()) {
            if (!m) continue;
            for (auto* member : m->members) {
                if (!member) continue;
                for (const auto& op : kObjectProps)
                    fulls.push_back("game.Workspace." + m->name + "." + member->GetName() + "." + op.name + ".value");
            }
        }
    }
}

struct ObjectBinding {
    std::string name;
    std::string object;
};

struct ScriptVarInfo {
    std::vector<std::string> names;
    std::vector<ObjectBinding> objectBindings;
};

std::string VarLower(const std::string& s) {
    std::string r = s;
    for (char& ch : r) ch = (char)tolower((unsigned char)ch);
    return r;
}

ScriptVarInfo CollectScriptVariables(const std::string& source) {
    ScriptVarInfo info;
    size_t i = 0;
    while (i < source.size()) {
        char c = source[i];
        if (c == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            while (i < source.size() && source[i] != '\n') ++i;
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < source.size() && source[i] != '"' && source[i] != '\n') ++i;
            ++i;
            continue;
        }
        if (c == '\n' || c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < source.size() && (isalnum((unsigned char)source[j]) || source[j] == '_')) ++j;
            std::string word = source.substr(i, j - i);
            if (word != "var" && word != "global") { i = j; continue; }

            size_t k = j;
            while (k < source.size() && (source[k] == ' ' || source[k] == '\t')) ++k;
            if (k >= source.size() || !(isalpha((unsigned char)source[k]) || source[k] == '_')) {
                i = j;
                continue;
            }
            size_t nEnd = k;
            while (nEnd < source.size() && (isalnum((unsigned char)source[nEnd]) || source[nEnd] == '_')) ++nEnd;
            std::string name = source.substr(k, nEnd - k);
            if (std::find(info.names.begin(), info.names.end(), name) == info.names.end())
                info.names.push_back(name);

            size_t m = nEnd;
            while (m < source.size() && (source[m] == ' ' || source[m] == '\t')) ++m;
            if (m < source.size() && source[m] == '=') {
                ++m;
                while (m < source.size() && (source[m] == ' ' || source[m] == '\t')) ++m;
                auto nextSeg = [&](size_t& p) -> std::string {
                    while (p < source.size() && (source[p] == ' ' || source[p] == '\t')) ++p;
                    if (p >= source.size()) return "";
                    if (isalpha((unsigned char)source[p]) || source[p] == '_') {
                        size_t s = p;
                        while (p < source.size() && (isalnum((unsigned char)source[p]) || source[p] == '_')) ++p;
                        return source.substr(s, p - s);
                    }
                    if (source[p] == '[') {
                        ++p;
                        if (p < source.size() && source[p] == '"') {
                            ++p;
                            size_t s = p;
                            while (p < source.size() && source[p] != '"' && source[p] != '\n') ++p;
                            std::string seg = source.substr(s, p - s);
                            if (p < source.size() && source[p] == '"') ++p;
                            if (p < source.size() && source[p] == ']') ++p;
                            return seg;
                        }
                    }
                    return "";
                };
                std::string s0 = nextSeg(m);
                if (!s0.empty() && m < source.size() && source[m] == '.') {
                    ++m;
                    std::string s1 = nextSeg(m);
                    if (!s1.empty() && m < source.size() && source[m] == '.') {
                        ++m;
                        std::string s2 = nextSeg(m);
                        if (!s2.empty() && VarLower(s0) == "game" && VarLower(s1) == "workspace") {
                            std::string object = s2;
                            if (m < source.size() && source[m] == '.') {
                                ++m;
                                std::string s3 = nextSeg(m);
                                if (!s3.empty()) object += "." + s3;
                            }
                            bool exists = false;
                            for (const auto &b: info.objectBindings)
                                if (b.name == name) { exists = true; break; }
                            if (!exists) info.objectBindings.push_back({ name, object });
                        }
                    } else if (VarLower(s0) == "instance" && VarLower(s1) == "new" &&
                               m < source.size() && source[m] == '(') {
                        ++m;
                        while (m < source.size() && source[m] == ' ') ++m;
                        if (m < source.size() && source[m] == '"') {
                            ++m;
                            size_t s = m;
                            while (m < source.size() && source[m] != '"' && source[m] != '\n') ++m;
                            std::string shapeName = source.substr(s, m - s);
                            if (m < source.size() && source[m] == '"') ++m;
                            while (m < source.size() && source[m] == ' ') ++m;
                            if (m < source.size() && source[m] == ')') ++m;
                            bool exists = false;
                            for (const auto &b: info.objectBindings)
                                if (b.name == name) { exists = true; break; }
                            if (!exists) info.objectBindings.push_back({ name, shapeName });
                        }
                    }
                }
            }
            i = nEnd;
            continue;
        }
        ++i;
    }
    return info;
}

}

namespace {

bool IsIdentChar(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

struct CompletionWord {
    size_t wordStart = 0;
    std::string partial;
    std::string chain;
    bool isMember = false;
};

CompletionWord AnalyzeCompletionWord(const std::string& text) {
    CompletionWord w;
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
    size_t start = end;
    while (start > 0 && IsIdentChar(text[start - 1])) --start;
    w.wordStart = start;
    w.partial = text.substr(start, end - start);

    if (start > 0 && text[start - 1] == '.') {
        w.isMember = true;
        size_t p = start - 1;
        while (p > 0) {
            char c = text[p - 1];
            if (IsIdentChar(c)) { --p; continue; }
            if (c == '.' && p >= 2 && IsIdentChar(text[p - 2])) { --p; continue; }
            break;
        }
        w.chain = text.substr(p, (start - 1) - p);
    }
    return w;
}

bool IsBoolPropertyLhs(const std::string& lhs) {
    for (const auto& kp : kKnownProps) {
        std::string full = std::string(kp.path) + ".value";
        if (kp.isBool && (lhs == full || lhs == kp.path)) return true;
    }
    std::string propPath = lhs;
    if (propPath.size() >= 6 && propPath.compare(propPath.size() - 6, 6, ".value") == 0)
        propPath.resize(propPath.size() - 6);
    size_t lastDot = propPath.rfind('.');
    std::string prop = lastDot == std::string::npos ? propPath : propPath.substr(lastDot + 1);
    for (const auto& op : kObjectProps)
        if (op.isBool && prop == op.name) return true;
    return false;
}

bool IsEnumPropertyLhs(const std::string& lhs) {
    std::string propPath = lhs;
    if (propPath.size() >= 6 && propPath.compare(propPath.size() - 6, 6, ".value") == 0)
        propPath.resize(propPath.size() - 6);
    size_t lastDot = propPath.rfind('.');
    std::string prop = lastDot == std::string::npos ? propPath : propPath.substr(lastDot + 1);
    return prop == "CollisionAccuracy";
}

void CollectScriptCompletions(const std::string& scriptSource, ScriptVarInfo& vars) {
    vars = CollectScriptVariables(scriptSource);
    if (IsRuntimeReady()) {
        for (const auto& kv : GetRuntime().globalVars) {
            if (std::find(vars.names.begin(), vars.names.end(), kv.first) == vars.names.end())
                vars.names.push_back(kv.first);
        }
    }
}

void PushUnique(std::vector<std::string>& result, const std::string& s) {
    if (std::find(result.begin(), result.end(), s) == result.end())
        result.push_back(s);
}

}

std::vector<std::string> GetCompletions(const std::string& input,
                                        const std::string& scriptSource) {
    std::vector<std::string> result;
    if (Trim(input).empty()) return result;

    ScriptVarInfo vars;
    if (!scriptSource.empty()) CollectScriptCompletions(scriptSource, vars);

    size_t eq = input.find('=');
    std::string lhsPart = eq == std::string::npos ? std::string() : input.substr(0, eq);
    // Trim the RHS so a space after '=' doesn't leak into the chain.
    std::string target = eq == std::string::npos ? input : Trim(input.substr(eq + 1));

    const CompletionWord word = AnalyzeCompletionWord(target);

    if (eq != std::string::npos && IsBoolPropertyLhs(Trim(lhsPart))) {
        const std::string& p = word.partial;
        if ((p.empty() || CIStartsWith("true", p)) && p != "true") result.push_back("true");
        if ((p.empty() || CIStartsWith("false", p)) && p != "false") result.push_back("false");
        return result;
    }

    // Enum completions (only after a dot — bare "enum" is handled as a keyword
    // below).  AcceptCompletion always replaces from the last dot, so we return
    // just the next segment (e.g. "CollisionAccuracy"), not the full path.
    if (eq != std::string::npos && IsEnumPropertyLhs(Trim(lhsPart)) && word.isMember) {
        const std::string& partial = word.partial;
        // After "enum."  → offer type names
        if (CIStartsWith(word.chain, "enum") && word.chain.size() == 4) {
            const char* names[] = { "CollisionAccuracy" };
            for (const char* n : names)
                if (CIStartsWith(n, partial) && n != partial)
                    PushUnique(result, n);
        }
        // After "enum.CollisionAccuracy."  → offer member names
        if (CIStartsWith(word.chain, "enum.CollisionAccuracy") && word.chain.size() == 22) {
            const char* members[] = { "Box", "Hull", "Default", "Precise" };
            for (const char* m : members)
                if (CIStartsWith(m, partial) && m != partial)
                    PushUnique(result, m);
        }
        return result;
    }

    if (word.partial.empty() && !word.isMember) return result;

    if (word.isMember) {
        std::vector<std::string> fulls;
        CollectFullPaths(fulls);
        for (const auto& b : vars.objectBindings)
            for (const auto& op : kObjectProps)
                fulls.push_back(b.name + "." + op.name + ".value");
        for (const auto& op : kObjectProps)
            fulls.push_back(std::string("self.") + op.name + ".value");
        fulls.push_back("tick.wait");
        fulls.push_back("Vector3.new");
        fulls.push_back("Vector3");
        fulls.push_back("instance.new");
        fulls.push_back("instance");
        fulls.push_back("new");

        // Enum types and members (when the user starts typing "enum.Colli" etc.)
        if (CIStartsWith(word.chain, "enum")) {
            fulls.push_back("enum.CollisionAccuracy");
            const char* accMembers[] = { "Box", "Hull", "Default", "Precise" };
            for (const char* m : accMembers)
                fulls.push_back(std::string("enum.CollisionAccuracy.") + m);
        }

        const std::string& base = word.chain;
        const std::string& partial = word.partial;
        for (const auto& full : fulls) {
            if (full.size() < base.size() + 1) continue;
            if (full.compare(0, base.size(), base) != 0) continue;
            if (full[base.size()] != '.') continue;
            size_t nextStart = base.size() + 1;
            size_t nextDot = full.find('.', nextStart);
            std::string cand = full.substr(nextStart,
                nextDot == std::string::npos ? std::string::npos : nextDot - nextStart);
            if (cand != partial && CIStartsWith(cand, partial))
                PushUnique(result, cand);
        }
        return result;
    }

    const std::string& partial = word.partial;
    const std::vector<std::string> keywords = {
        "for", "while", "if", "else", "not", "and", "or",
        "every", "print", "rgb",
        "self", "var", "global", "true", "false", "game", "tick", "Vector3",
        "instance", "new", "wait", "enum", "Transparency"
    };
    for (const auto& kw : keywords) {
        if (kw != partial && CIStartsWith(kw, partial)) PushUnique(result, kw);
    }
    for (const auto& name : vars.names) {
        if (name != partial && CIStartsWith(name, partial)) PushUnique(result, name);
    }
    return result;
}

size_t CompletionWordStart(const std::string& textUpToCaret) {
    return AnalyzeCompletionWord(textUpToCaret).wordStart;
}

namespace {

enum class TokenType { Ident, Number, String, Punct };

struct Token {
    TokenType type = TokenType::Ident;
    std::string text;
    int line = 1;
};

bool IsPunct(char c) {
    return c == '.' || c == '=' || c == '(' || c == ')' || c == '{' || c == '}' ||
           c == '[' || c == ']' || c == ',' || c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '!';
}

std::vector<Token> Tokenize(const std::string& src, std::string& outError) {
    std::vector<Token> toks;
    size_t i = 0;
    int line = 1;
    while (i < src.size()) {
        char c = src[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        if (c == '-' && i + 1 < src.size() && src[i + 1] == '-') {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        if (c == '"') {
            size_t j = i + 1;
            std::string s;
            while (j < src.size() && src[j] != '"') {
                if (src[j] == '\n') ++line;
                s.push_back(src[j]);
                ++j;
            }
            if (j >= src.size()) { outError = "Unterminated string on line " + std::to_string(line); return {}; }
            toks.push_back({ TokenType::String, s, line });
            i = j + 1;
            continue;
        }
        if (isdigit((unsigned char)c)) {
            size_t j = i;
            while (j < src.size() && (isdigit((unsigned char)src[j]) || src[j] == '.')) ++j;
            toks.push_back({ TokenType::Number, src.substr(i, j - i), line });
            i = j;
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t j = i;
            while (j < src.size() && (isalnum((unsigned char)src[j]) || src[j] == '_')) ++j;
            toks.push_back({ TokenType::Ident, src.substr(i, j - i), line });
            i = j;
            continue;
        }
        if (IsPunct(c)) {
            if (i + 1 < src.size() && src[i + 1] == '=' &&
                (c == '=' || c == '!')) {
                toks.push_back({ TokenType::Punct, src.substr(i, 2), line });
                i += 2;
                continue;
            }
            toks.push_back({ TokenType::Punct, std::string(1, c), line });
            ++i;
            continue;
        }
        outError = std::string("Unexpected character '") + c + "' on line " + std::to_string(line);
        return {};
    }
    return toks;
}

enum class BlockKind { Every, Count, While, If };

struct Expr {
    enum class Kind { Number, Bool, String, Tuple, Color, Var, Path, Neg, Not, Binary, Call };
    Kind kind = Kind::Number;
    float number = 0.0f;
    bool boolean = false;
    std::string text;
    std::vector<std::string> pathSegs;
    std::string op;
    std::vector<Expr> items;
    int line = 1;
};

struct Stmt {
    enum class Type { Assign, VarDecl, Wait, Block, Print, ExprStmt };
    Type type = Type::Assign;
    std::string path;
    std::string property;
    std::string varName;
    bool isGlobal = false;
    bool isDeclaration = false;
    Expr value;
    Expr condition;
    float seconds = 0.0f;
    BlockKind kind = BlockKind::Count;
    int count = 0;
    std::vector<Stmt> body;
    std::vector<Stmt> elseBody;
    int line = 1;
};

struct Program {
    std::vector<Stmt> body;
    std::string error;
    int errorLine = 0;
    bool ok() const { return error.empty(); }
};

struct Parser {
    std::vector<Token> toks;
    size_t pos = 0;
    std::string error;
    int errorLine = 0;

    bool AtEnd() const { return pos >= toks.size(); }

    const Token* Peek(size_t ahead = 0) const {
        return pos + ahead < toks.size() ? &toks[pos + ahead] : nullptr;
    }

    bool IsPunctToken(const std::string& p) const {
        const Token* t = Peek();
        return t && t->type == TokenType::Punct && t->text == p;
    }

    bool IsIdentToken(const std::string& w) const {
        const Token* t = Peek();
        return t && t->type == TokenType::Ident && t->text == w;
    }

    bool ExpectPunct(const std::string& p) {
        const Token* t = Peek();
        if (t && t->type == TokenType::Punct && t->text == p) { ++pos; return true; }
        Fail("Expected '" + p + "'", t ? t->line : errorLine);
        return false;
    }

    void Fail(const std::string& msg, int line) {
        if (error.empty()) { error = msg; errorLine = line; }
    }

    std::vector<Stmt> ParseBody() {
        std::vector<Stmt> body;
        while (true) {
            if (AtEnd()) { Fail("Expected '}'", errorLine); break; }
            if (IsPunctToken("}")) { ++pos; break; }
            Stmt s;
            if (!ParseStmt(s)) break;
            body.push_back(std::move(s));
        }
        return body;
    }

    bool ParseExprStmt(Stmt& out) {
        int line = Peek()->line;
        Expr e;
        if (!ParseExpr(e)) return false;
        out.type = Stmt::Type::ExprStmt;
        out.value = std::move(e);
        out.line = line;
        return true;
    }

    bool ParseStmt(Stmt& out) {
        const Token* t = Peek();
        if (!t) { Fail("Unexpected end of script", errorLine); return false; }
        if (t->type == TokenType::Ident) {
            if (t->text == "for") return ParseFor(out);
            if (t->text == "while") return ParseWhile(out);
            if (t->text == "if") return ParseIf(out);
            if (t->text == "tick") return ParseTickWait(out);
            if (t->text == "print") return ParsePrint(out);
            if (t->text == "var") return ParseVarDecl(out, false);
            if (t->text == "global") return ParseVarDecl(out, true);
            if (t->text == "instance") {
                const Token* n1 = Peek(1);
                const Token* n2 = Peek(2);
                if (n1 && n1->type == TokenType::Punct && n1->text == "." &&
                    n2 && n2->type == TokenType::Ident && n2->text == "new")
                    return ParseExprStmt(out);
            }
        }
        return ParseAssign(out);
    }

    static bool IsReservedWord(const std::string& w) {
        return w == "true" || w == "false" || w == "self" || w == "var" || w == "global" ||
               w == "for" || w == "while" || w == "every" || w == "instance" ||
               w == "tick" || w == "print" || w == "if" || w == "else" ||
               w == "not" || w == "and" || w == "or";
    }

    bool ParsePathSegments(std::vector<std::string>& segs, int line) {
        const Token* t = Peek();
        if (!t || t->type != TokenType::Ident)
            return Fail("Unexpected token", t ? t->line : line), false;
        ++pos;
        segs.push_back(t->text);
        while (true) {
            if (!IsPunctToken(".")) break;
            ++pos;
            const Token* n = Peek();
            if (n && n->type == TokenType::Punct && n->text == "[") {
                ++pos;
                const Token* s = Peek();
                if (!s || s->type != TokenType::String)
                    return Fail("Expected a name string after '['", n->line), false;
                ++pos;
                if (!ExpectPunct("]")) return false;
                segs.push_back(s->text);
            } else if (n && n->type == TokenType::Ident) {
                ++pos;
                segs.push_back(n->text);
            } else {
                return Fail("Expected a name after '.'", n ? n->line : line), false;
            }
        }
        return true;
    }

    bool ParseFor(Stmt& out) {
        int line = Peek()->line;
        ++pos;
        const Token* t = Peek();
        if (!t) return Fail("Expected 'every' or a number after 'for'", line), false;
        if (t->type == TokenType::Ident && t->text == "every") {
            ++pos;
            const Token* n = Peek();
            if (!n || n->type != TokenType::Ident || n->text != "instance")
                return Fail("Expected 'instance' after 'for every'", line), false;
            ++pos;
            out.kind = BlockKind::Every;
        } else if (t->type == TokenType::Number) {
            ++pos;
            out.kind = BlockKind::Count;
            out.count = atoi(t->text.c_str());
            if (out.count < 0) return Fail("Loop count must be 0 or greater", line), false;
        } else {
            return Fail("Expected 'every' or a number after 'for'", line), false;
        }
        if (!ExpectPunct("{")) return false;
        out.line = line;
        out.type = Stmt::Type::Block;
        out.body = ParseBody();
        return error.empty();
    }

    bool ParseWhile(Stmt& out) {
        int line = Peek()->line;
        ++pos;
        Expr cond;
        if (!ParseExpr(cond)) return false;
        if (!ExpectPunct("{")) return false;
        out.line = line;
        out.type = Stmt::Type::Block;
        out.kind = BlockKind::While;
        out.condition = std::move(cond);
        out.body = ParseBody();
        return error.empty();
    }

    bool ParseIf(Stmt& out) {
        int line = Peek()->line;
        ++pos;
        Expr cond;
        if (!ParseExpr(cond)) return false;
        if (!ExpectPunct("{")) return false;
        out.line = line;
        out.type = Stmt::Type::Block;
        out.kind = BlockKind::If;
        out.condition = std::move(cond);
        out.body = ParseBody();
        if (!error.empty()) return false;

        const Token* n = Peek();
        if (n && n->type == TokenType::Ident && n->text == "else") {
            ++pos;
            if (IsPunctToken("{")) {
                ++pos;
                out.elseBody = ParseBody();
            } else {
                const Token* e = Peek();
                if (e && e->type == TokenType::Ident && e->text == "if") {
                    Stmt inner;
                    if (!ParseIf(inner)) return false;
                    out.elseBody.push_back(std::move(inner));
                } else {
                    return Fail("Expected '{' or 'if' after 'else'", n->line), false;
                }
            }
        }
        return error.empty();
    }

    bool ParseTickWait(Stmt& out) {
        int line = Peek()->line;
        ++pos;
        if (!ExpectPunct(".")) return false;
        const Token* w = Peek();
        if (!w || w->type != TokenType::Ident || w->text != "wait")
            return Fail("Expected 'tick.wait(seconds)'", line), false;
        ++pos;
        if (!ExpectPunct("(")) return false;
        bool negative = false;
        if (IsPunctToken("-")) { negative = true; ++pos; }
        const Token* n = Peek();
        if (!n || n->type != TokenType::Number)
            return Fail("tick.wait needs a number of seconds", n ? n->line : line), false;
        ++pos;
        out.seconds = (float)atof(n->text.c_str());
        if (negative) out.seconds = -out.seconds;
        if (out.seconds < 0.0f) return Fail("tick.wait seconds cannot be negative", n->line), false;
        if (!ExpectPunct(")")) return false;
        out.line = line;
        out.type = Stmt::Type::Wait;
        return true;
    }

    bool ParsePrint(Stmt& out) {
        int line = Peek()->line;
        ++pos;
        if (!ExpectPunct("(")) return false;
        Expr e;
        if (!ParseExpr(e)) return false;
        if (!ExpectPunct(")")) return false;
        out.line = line;
        out.type = Stmt::Type::Print;
        out.value = std::move(e);
        return true;
    }

    bool ParseVarDecl(Stmt& out, bool isGlobal) {
        int line = Peek()->line;
        ++pos;
        const Token *n = Peek();
        if (!n || n->type != TokenType::Ident)
            return Fail(isGlobal ? "Expected a variable name after 'global'" : "Expected a variable name after 'var'", n ? n->line : line), false;
        if (IsReservedWord(n->text))
            return Fail("'" + n->text + "' is a reserved word and cannot be a variable name", n->line), false;
        ++pos;
        if (!ExpectPunct("=")) return false;
        Expr e;
        if (!ParseExpr(e)) return false;
        out.line = line;
        out.type = Stmt::Type::VarDecl;
        out.varName = n->text;
        out.isGlobal = isGlobal;
        out.isDeclaration = true;
        out.value = std::move(e);
        return true;
    }

    bool ParseAssign(Stmt& out) {
        int line = Peek()->line;
        std::vector<std::string> segs;
        if (!ParsePathSegments(segs, line)) return false;

        if (segs.size() == 1 && IsPunctToken("=")) {
            if (IsReservedWord(segs[0]))
                return Fail("'" + segs[0] + "' is a reserved word and cannot be a variable name", line), false;
            ++pos;
            Expr e;
            if (!ParseExpr(e)) return false;
            out.line = line;
            out.type = Stmt::Type::VarDecl;
            out.varName = segs[0];
            out.isDeclaration = false;
            out.value = std::move(e);
            return true;
        }

        if (segs.size() < 2 || segs.back() != "value")
            return Fail("Expected 'value' at the end of the property path", line), false;
        if (!ExpectPunct("=")) return false;
        Expr e;
        if (!ParseExpr(e)) return false;
        out.line = line;
        out.type = Stmt::Type::Assign;
        out.property = "value";
        std::string path;
        for (size_t i = 0; i + 1 < segs.size(); ++i) {
            if (i) path += '.';
            path += segs[i];
        }
        out.path = path;
        out.value = std::move(e);
        return true;
    }

    bool ParsePrimary(Expr& out) {
        const Token* t = Peek();
        if (!t) return Fail("Expected a value", errorLine), false;
        if (t->type == TokenType::Number) {
            ++pos;
            out.kind = Expr::Kind::Number;
            out.number = (float)atof(t->text.c_str());
            out.line = t->line;
            return true;
        }
        if (t->type == TokenType::String) {
            ++pos;
            out.kind = Expr::Kind::String;
            out.text = t->text;
            out.line = t->line;
            return true;
        }
        if (t->type == TokenType::Ident) {
            if (t->text == "true" || t->text == "false") {
                ++pos;
                out.kind = Expr::Kind::Bool;
                out.boolean = (t->text == "true");
                out.line = t->line;
                return true;
            }
            const Token* next = Peek(1);
            if (next && next->type == TokenType::Punct && next->text == "(") {
                int line = t->line;
                std::string callee = t->text;
                ++pos;
                if (!ExpectPunct("(")) return false;
                std::vector<Expr> args;
                if (!IsPunctToken(")")) {
                    do {
                        Expr e;
                        if (!ParseExpr(e)) return false;
                        args.push_back(std::move(e));
                        if (IsPunctToken(")")) break;
                        if (!ExpectPunct(",")) return false;
                    } while (true);
                }
                if (!ExpectPunct(")")) return false;
                out.kind = Expr::Kind::Call;
                out.text = callee;
                out.items = std::move(args);
                out.line = line;
                return true;
            }
            {
                const Token* dot = Peek(1);
                const Token* fn = Peek(2);
                const Token* paren = Peek(3);
                std::string first = t->text;
                for (char& ch : first) ch = (char)tolower((unsigned char)ch);
                if (first == "vector3" && dot && dot->type == TokenType::Punct && dot->text == "." &&
                    fn && fn->type == TokenType::Ident && fn->text == "new" &&
                    paren && paren->type == TokenType::Punct && paren->text == "(") {
                    int line = t->line;
                    pos += 3;
                    if (!ExpectPunct("(")) return false;
                    std::vector<Expr> args;
                    if (!IsPunctToken(")")) {
                        do {
                            Expr e;
                            if (!ParseExpr(e)) return false;
                            args.push_back(std::move(e));
                            if (IsPunctToken(")")) break;
                            if (!ExpectPunct(",")) return false;
                        } while (true);
                    }
                    if (!ExpectPunct(")")) return false;
                    out.kind = Expr::Kind::Call;
                    out.text = "Vector3.new";
                    out.items = std::move(args);
                    out.line = line;
                    return true;
                }
                if (first == "instance" && dot && dot->type == TokenType::Punct && dot->text == "." &&
                    fn && fn->type == TokenType::Ident && fn->text == "new" &&
                    paren && paren->type == TokenType::Punct && paren->text == "(") {
                    int line = t->line;
                    pos += 3;
                    if (!ExpectPunct("(")) return false;
                    std::vector<Expr> args;
                    if (!IsPunctToken(")")) {
                        do {
                            Expr e;
                            if (!ParseExpr(e)) return false;
                            args.push_back(std::move(e));
                            if (IsPunctToken(")")) break;
                            if (!ExpectPunct(",")) return false;
                        } while (true);
                    }
                    if (!ExpectPunct(")")) return false;
                    out.kind = Expr::Kind::Call;
                    out.text = "Instance.new";
                    out.items = std::move(args);
                    out.line = line;
                    return true;
                }
            }
            std::vector<std::string> segs;
            if (!ParsePathSegments(segs, t->line)) return false;
            if (segs.size() == 1) {
                out.kind = Expr::Kind::Var;
                out.text = segs[0];
            } else {
                out.kind = Expr::Kind::Path;
                out.pathSegs = std::move(segs);
            }
            out.line = t->line;
            return true;
        }
        if (t->type == TokenType::Punct && t->text == "(") {
            int line = t->line;
            ++pos;
            Expr first;
            if (!ParseExpr(first)) return false;
            if (IsPunctToken(")")) {
                ++pos;
                out = std::move(first);
                return true;
            }
            std::vector<Expr> items;
            items.push_back(std::move(first));
            for (int i = 1; i < 3; ++i) {
                if (!IsPunctToken(","))
                    return Fail("Expected ',' between tuple values", Peek() ? Peek()->line : line), false;
                ++pos;
                Expr e;
                if (!ParseExpr(e)) return false;
                items.push_back(std::move(e));
            }
            if (!ExpectPunct(")")) return false;
            out.kind = Expr::Kind::Tuple;
            out.items = std::move(items);
            out.line = line;
            return true;
        }
        return Fail("Expected a value", t->line), false;
    }

    bool ParseUnary(Expr& out) {
        if (IsPunctToken("-")) {
            int line = Peek()->line;
            ++pos;
            Expr e;
            if (!ParseUnary(e)) return false;
            out.kind = Expr::Kind::Neg;
            out.line = line;
            out.items.push_back(std::move(e));
            return true;
        }
        return ParsePrimary(out);
    }

    bool ParseTerm(Expr& out) {
        if (!ParseUnary(out)) return false;
        while (IsPunctToken("*") || IsPunctToken("/")) {
            std::string op = Peek()->text;
            int line = Peek()->line;
            ++pos;
            Expr rhs;
            if (!ParseUnary(rhs)) return false;
            Expr bin;
            bin.kind = Expr::Kind::Binary;
            bin.op = op;
            bin.line = line;
            bin.items.push_back(std::move(out));
            bin.items.push_back(std::move(rhs));
            out = std::move(bin);
        }
        return true;
    }

    bool ParseAddSub(Expr& out) {
        if (!ParseTerm(out)) return false;
        while (IsPunctToken("+") || IsPunctToken("-")) {
            std::string op = Peek()->text;
            int line = Peek()->line;
            ++pos;
            Expr rhs;
            if (!ParseTerm(rhs)) return false;
            Expr bin;
            bin.kind = Expr::Kind::Binary;
            bin.op = op;
            bin.line = line;
            bin.items.push_back(std::move(out));
            bin.items.push_back(std::move(rhs));
            out = std::move(bin);
        }
        return true;
    }

    bool ParseComparison(Expr& out) {
        if (!ParseAddSub(out)) return false;
        while (IsPunctToken("==") || IsPunctToken("!=")) {
            std::string op = Peek()->text;
            int line = Peek()->line;
            ++pos;
            Expr rhs;
            if (!ParseAddSub(rhs)) return false;
            Expr bin;
            bin.kind = Expr::Kind::Binary;
            bin.op = op;
            bin.line = line;
            bin.items.push_back(std::move(out));
            bin.items.push_back(std::move(rhs));
            out = std::move(bin);
        }
        return true;
    }

    bool ParseNot(Expr& out) {
        const Token* t = Peek();
        if (t && t->type == TokenType::Ident && t->text == "not") {
            int line = t->line;
            ++pos;
            Expr e;
            if (!ParseNot(e)) return false;
            out.kind = Expr::Kind::Not;
            out.line = line;
            out.items.push_back(std::move(e));
            return true;
        }
        return ParseComparison(out);
    }

    bool ParseAnd(Expr& out) {
        if (!ParseNot(out)) return false;
        while (IsIdentToken("and")) {
            int line = Peek()->line;
            ++pos;
            Expr rhs;
            if (!ParseNot(rhs)) return false;
            Expr bin;
            bin.kind = Expr::Kind::Binary;
            bin.op = "and";
            bin.line = line;
            bin.items.push_back(std::move(out));
            bin.items.push_back(std::move(rhs));
            out = std::move(bin);
        }
        return true;
    }

    bool ParseOr(Expr& out) {
        if (!ParseAnd(out)) return false;
        while (IsIdentToken("or")) {
            int line = Peek()->line;
            ++pos;
            Expr rhs;
            if (!ParseAnd(rhs)) return false;
            Expr bin;
            bin.kind = Expr::Kind::Binary;
            bin.op = "or";
            bin.line = line;
            bin.items.push_back(std::move(out));
            bin.items.push_back(std::move(rhs));
            out = std::move(bin);
        }
        return true;
    }

    bool ParseExpr(Expr& out) {
        return ParseOr(out);
    }
};

Program Compile(const std::string& source) {
    Program prog;
    std::string tokenError;
    auto toks = Tokenize(source, tokenError);
    if (!tokenError.empty()) { prog.error = tokenError; return prog; }
    Parser p;
    p.toks = std::move(toks);
    while (!p.AtEnd()) {
        if (p.IsPunctToken("}")) { p.Fail("Unexpected '}'", p.Peek()->line); break; }
        Stmt s;
        if (!p.ParseStmt(s)) break;
        prog.body.push_back(std::move(s));
    }
    if (!p.error.empty()) { prog.error = p.error; prog.errorLine = p.errorLine; }
    return prog;
}

// ---------------------------------------------------------------------------
// Script safety guardrails: static analysis for dangerous loop patterns
// ---------------------------------------------------------------------------

static bool IsInstanceNewCall(const Expr& e) {
    if (e.kind != Expr::Kind::Call) return false;
    return e.text == "Instance.new";
}

static bool IsHeavyPropertyAssign(const std::string& prop) {
    return prop == "Position" || prop == "Size" || prop == "Rotation" ||
           prop == "Velocity" || prop == "AngularVelocity" || prop == "Mass";
}

static bool IsHeavyStmt(const Stmt& s) {
    if (s.type == Stmt::Type::ExprStmt && IsInstanceNewCall(s.value))
        return true;
    if (s.type == Stmt::Type::Assign && IsHeavyPropertyAssign(s.property))
        return true;
    if (s.type == Stmt::Type::Block) {
        for (const auto& child : s.body)
            if (IsHeavyStmt(child)) return true;
    }
    return false;
}

static bool HasWaitInBody(const std::vector<Stmt>& body) {
    for (const auto& s : body) {
        if (s.type == Stmt::Type::Wait) return true;
        if (s.type == Stmt::Type::Block && s.kind != BlockKind::While &&
            s.kind != BlockKind::Count && s.kind != BlockKind::Every) {
            if (HasWaitInBody(s.body)) return true;
        }
    }
    return false;
}

}

bool HasIgnoreFlag(const std::string& source) {
    size_t i = 0;
    while (i < source.size()) {
        if (source[i] == '-' && i + 1 < source.size() && source[i + 1] == '-') {
            size_t j = i + 2;
            while (j < source.size() && source[j] != '\n') ++j;
            std::string comment = source.substr(i + 2, j - (i + 2));
            size_t start = comment.find_first_not_of(" \t");
            size_t end = comment.find_last_not_of(" \t");
            if (start != std::string::npos &&
                comment.substr(start, end - start + 1) == "!crashcheck")
                return true;
            i = j;
            continue;
        }
        if (source[i] != ' ' && source[i] != '\t' && source[i] != '\r' &&
            source[i] != '\n')
            break;
        ++i;
    }
    return false;
}

std::vector<ScriptWarning> CheckScriptSafety(const std::string& scriptName,
                                             const std::string& source) {
    std::vector<ScriptWarning> warnings;
    if (HasIgnoreFlag(source)) return warnings;

    Program prog = Compile(source);
    if (!prog.ok()) return warnings;

    for (const auto& s : prog.body) {
        if (s.type == Stmt::Type::Block &&
            (s.kind == BlockKind::While || s.kind == BlockKind::Count ||
             s.kind == BlockKind::Every)) {
            if (IsHeavyStmt(s) && !HasWaitInBody(s.body)) {
                warnings.push_back({scriptName, s.line,
                    "Loop contains heavy operations without tick.wait()"});
            }
        }
    }
    return warnings;
}

namespace {

constexpr int RUN_BUDGET = 50000;

}

struct Runtime::Coroutine {
    struct Frame {
        const std::vector<Stmt>* body = nullptr;
        size_t pc = 0;
        BlockKind kind = BlockKind::Count;
        int count = 0;
        size_t instanceIdx = 0;
        std::vector<ScatteredObject*> instances;
        ScatteredObject* outerSelf = nullptr;
        const Expr* condition = nullptr;
        bool isProgram = false;
    };

    Program program;
    std::vector<Frame> stack;
    ScatteredObject* self = nullptr;
    double waitUntil = 0.0;
    bool runNextFrame = false;
    bool done = false;
    bool error = false;
    int errLine = 0;
    std::string errMsg;
    ScatteredObject* ownerObject = nullptr;
    int ownerIndex = -1;
    std::unordered_map<std::string, Value> vars;
};

namespace {

std::string FormatNumber(float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return buf;
}

std::string JoinPath(const std::vector<std::string>& segs) {
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) out += '.';
        out += segs[i];
    }
    return out;
}

Value MakeNumber(float v) { Value val; val.type = Value::Type::Number; val.number = v; return val; }
Value MakeBool(bool b)    { Value val; val.type = Value::Type::Bool;   val.boolean = b;  return val; }
Value MakeString(const std::string& s) { Value val; val.type = Value::Type::String; val.str = s; return val; }
Value MakeTuple(float x, float y, float z) { Value val; val.type = Value::Type::Tuple; val.tuple = { x, y, z }; return val; }
Value MakeTuple(const Vector3& v) { return MakeTuple(v.x, v.y, v.z); }
Value MakeColor(const Color& c)  { Value val; val.type = Value::Type::Color; val.color = c; return val; }
Value MakeObject(ScatteredObject* o) { Value val; val.type = Value::Type::Object; val.object = o; return val; }
Value MakeEnum(const std::string& name, int ordinal) { Value val; val.type = Value::Type::Enum; val.enumName = name; val.ordinal = ordinal; return val; }

std::string EnumMemberName(const std::string& enumName, int ordinal);

bool ValueToAssignmentString(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Type::Number: out = FormatNumber(v.number); return true;
        case Value::Type::Bool:   out = v.boolean ? "true" : "false"; return true;
        case Value::Type::String: out = "\"" + v.str + "\""; return true;
        case Value::Type::Tuple:  out = "(" + FormatNumber(v.tuple.x) + ", " + FormatNumber(v.tuple.y) + ", " + FormatNumber(v.tuple.z) + ")"; return true;
        case Value::Type::Color:  out = "(" + FormatNumber((float)v.color.r) + ", " + FormatNumber((float)v.color.g) + ", " + FormatNumber((float)v.color.b) + ")"; return true;
        case Value::Type::Enum:   out = "enum." + v.enumName + "." + EnumMemberName(v.enumName, v.ordinal); return true;
        case Value::Type::Object: return false;
    }
    return false;
}

std::string FormatValue(const Value& v) {
    switch (v.type) {
        case Value::Type::Number: return FormatNumber(v.number);
        case Value::Type::Bool:   return v.boolean ? "true" : "false";
        case Value::Type::String: return v.str;
        case Value::Type::Tuple:  return "(" + FormatNumber(v.tuple.x) + ", " + FormatNumber(v.tuple.y) + ", " + FormatNumber(v.tuple.z) + ")";
        case Value::Type::Color:  return "rgb(" + FormatNumber((float)v.color.r) + ", " + FormatNumber((float)v.color.g) + ", " + FormatNumber((float)v.color.b) + ")";
        case Value::Type::Object: return v.object ? v.object->GetName() : "<deleted object>";
        case Value::Type::Enum:   return "enum." + v.enumName + "." + EnumMemberName(v.enumName, v.ordinal);
    }
    return "";
}

bool GetVarValue(std::unordered_map<std::string, Value>& localVars, Runtime& rt,
                 const std::string& name, Value& out) {
    auto local = localVars.find(name);
    if (local != localVars.end()) { out = local->second; return true; }
    auto global = rt.globalVars.find(name);
    if (global != rt.globalVars.end()) { out = global->second; return true; }
    return false;
}

const char* TypeName(Value::Type t) {
    switch (t) {
        case Value::Type::Number: return "number";
        case Value::Type::Bool:   return "boolean";
        case Value::Type::String: return "string";
        case Value::Type::Tuple:  return "tuple";
        case Value::Type::Color:  return "color";
        case Value::Type::Object: return "object";
        case Value::Type::Enum:   return "enum";
    }
    return "value";
}

bool ValuesEqual(const Value& l, const Value& r, bool& outEq, std::string& err) {
    if (l.type != r.type) {
        err = std::string("Cannot compare ") + TypeName(l.type) + " with " + TypeName(r.type);
        return false;
    }
    switch (l.type) {
        case Value::Type::Number: outEq = (l.number == r.number); return true;
        case Value::Type::Bool:   outEq = (l.boolean == r.boolean); return true;
        case Value::Type::String: outEq = (l.str == r.str); return true;
        case Value::Type::Tuple:  outEq = (l.tuple.x == r.tuple.x && l.tuple.y == r.tuple.y && l.tuple.z == r.tuple.z); return true;
        case Value::Type::Color:  outEq = (l.color.r == r.color.r && l.color.g == r.color.g && l.color.b == r.color.b && l.color.a == r.color.a); return true;
        case Value::Type::Object: outEq = (l.object == r.object); return true;
        case Value::Type::Enum:   outEq = (l.enumName == r.enumName && l.ordinal == r.ordinal); return true;
    }
    return false;
}

// Returns the member name for an enum value (used by FormatValue and
// ValueToAssignmentString). Unknown enums fall back to a decimal ordinal.
std::string EnumMemberName(const std::string& enumName, int ordinal) {
    if (enumName == "CollisionAccuracy") {
        for (int i = 0; i <= static_cast<int>(pcoll::CollisionAccuracy::Precise); ++i) {
            if (i == ordinal) return pcoll::CollisionAccuracyName(static_cast<pcoll::CollisionAccuracy>(i));
        }
    }
    char buf[16];
    sprintf_s(buf, "%d", ordinal);
    return buf;
}

std::string Lower(const std::string& s) {
    std::string r = s;
    for (char& ch : r) ch = (char)tolower((unsigned char)ch);
    return r;
}

bool EvalPathExpr(const std::vector<std::string>& segs,
                  std::unordered_map<std::string, Value>& localVars, ScatteredObject* self,
                  Runtime& rt, Value& out, std::string& err) {
    std::vector<std::string> path = segs;
    if (path.empty()) { err = "Empty path"; return false; }

    if (path.size() >= 2 && path[0] != "self" && path[0] != "game" && Lower(path[0]) != "enum") {
        Value v0;
        if (GetVarValue(localVars, rt, path[0], v0)) {
            if (v0.type != Value::Type::Object) {
                err = "Variable '" + path[0] + "' is not an object";
                return false;
            }
            const auto& objs = rt.GetObjects();
            if (!v0.object || std::find(objs.begin(), objs.end(), v0.object) == objs.end()) {
                err = "Object referenced by '" + path[0] + "' no longer exists";
                return false;
            }
            std::vector<std::string> expanded = { "game", "Workspace", v0.object->GetName() };
            for (size_t i = 1; i < path.size(); ++i) expanded.push_back(path[i]);
            path = std::move(expanded);
        } else {
            err = "Unknown variable '" + path[0] + "'";
            return false;
        }
    }

    bool hasValueSuffix = !path.empty() && path.back() == "value";
    std::vector<std::string> base = path;
    if (hasValueSuffix) base.pop_back();

    if (!base.empty() && Lower(base[0]) == "enum") {
        if (base.size() != 3) { err = "Expected 'enum.<EnumName>.<Member>'"; return false; }
        if (base[1] == "CollisionAccuracy") {
            pcoll::CollisionAccuracy acc;
            if (!pcoll::ParseCollisionAccuracy("enum.CollisionAccuracy." + base[2], acc)) {
                err = "Unknown enum member 'enum.CollisionAccuracy." + base[2] + "'";
                return false;
            }
            out = MakeEnum("CollisionAccuracy", static_cast<int>(acc));
            return true;
        }
        err = "Unknown enum '" + base[1] + "'";
        return false;
    }

    if (base.size() >= 3 && Lower(base[0]) == "game" && Lower(base[1]) == "lighting") {
        std::string p = Lower(base[2]);
        if (p == "globalshadows") { out = MakeBool(gfx::IsShadowsEnabled()); return true; }
        if (p == "shadowquality") { out = MakeNumber((float)gfx::GetShadowQuality()); return true; }
        if (p == "ambient")       { out = MakeNumber(gfx::GetAmbientIntensity()); return true; }
    }
    if (base.size() >= 3 && Lower(base[0]) == "game" && Lower(base[1]) == "rendering") {
        std::string p = Lower(base[2]);
        if (p == "grid")      { out = MakeBool(gfx::IsGridVisible()); return true; }
        if (p == "wireframe") { out = MakeBool(gfx::IsWireframe()); return true; }
    }
    if (base.size() >= 3 && Lower(base[0]) == "game" && Lower(base[1]) == "camera") {
        if (Lower(base[2]) == "fov") { out = MakeNumber(rt.GetCamera().fovy); return true; }
    }
    if (base.size() >= 3 && Lower(base[0]) == "game" && Lower(base[1]) == "physics") {
        std::string p = Lower(base[2]);
        if (p == "gravity")     { out = MakeNumber(rt.GetPhysicsGravity()); return true; }
        if (p == "friction")    { out = MakeNumber(rt.GetPhysicsFriction()); return true; }
        if (p == "restitution") { out = MakeNumber(rt.GetPhysicsRestitution()); return true; }
    }

    ScatteredObject* target = nullptr;
    std::string prop;
    if (base.size() >= 3 && Lower(base[0]) == "game" && Lower(base[1]) == "workspace") {
        const std::string& first = base[2];
        ModelGroup* model = FindModelByName(rt, first);
        if (model) {
            if (base.size() == 3) {
                err = "'" + first + "' is a model. Reference a part, e.g. game.Workspace." +
                      first + ".PartName";
                return false;
            }
            const std::string& part = base[3];
            for (auto* m : model->members) {
                if (m && m->GetName() == part) { target = m; break; }
            }
            if (!target) {
                err = "Cannot find part '" + part + "' in model '" + first + "'";
                return false;
            }
            if (base.size() > 4) prop = base[4];
            if (base.size() > 5) {
                err = "Too many path segments in '" + JoinPath(path) + "'";
                return false;
            }
        } else {
            target = rt.FindByName(first);
            if (!target) { err = "Cannot find object '" + first + "' in the workspace"; return false; }
            if (base.size() == 4) prop = base[3];
            else if (base.size() > 4) {
                err = "Object '" + first + "' has no member '" + base[3] + "'";
                return false;
            }
        }
    } else if (base.size() == 2 && base[0] == "self") {
        if (!self) { err = "No 'self' object in this script context"; return false; }
        target = self;
        prop = base[1];
    } else {
        err = "Unknown path '" + JoinPath(path) + "'";
        return false;
    }

    if (!prop.empty()) {
        if (prop == "Position")        { out = MakeTuple(*target->GetPosPtr()); return true; }
        if (prop == "Size")            { out = MakeTuple(*target->GetSizePtr()); return true; }
        if (prop == "Rotation")        { out = MakeTuple(*target->GetRotationPtr()); return true; }
        if (prop == "Origin")          { out = MakeTuple(*target->GetOriginPtr()); return true; }
        if (prop == "Color")           { Color col = *target->GetColorPtr(); out = MakeTuple((float)col.r, (float)col.g, (float)col.b); return true; }
        if (prop == "Velocity")        { out = MakeTuple(target->GetVelocity()); return true; }
        if (prop == "AngularVelocity") { out = MakeTuple(target->GetAngularVelocity()); return true; }
        if (prop == "Anchored")        { out = MakeBool(target->anchored); return true; }
        if (prop == "CanCollide")      { out = MakeBool(target->canCollide); return true; }
        if (prop == "CollisionAccuracy") { out = MakeEnum("CollisionAccuracy", static_cast<int>(target->GetCollisionAccuracy())); return true; }
        if (prop == "Mass")            { out = MakeNumber(target->GetMass()); return true; }
        if (prop == "Transparency")    { out = MakeNumber(target->GetTransparency()); return true; }
        err = "Unknown property '" + prop + "'";
        return false;
    }

    out = MakeObject(target);
    return true;
}

bool EvalExpr(const Expr& e, std::unordered_map<std::string, Value>& localVars,
              ScatteredObject* self, Runtime& rt, Value& out, std::string& err) {
    switch (e.kind) {
        case Expr::Kind::Number: out = MakeNumber(e.number); return true;
        case Expr::Kind::Bool:   out = MakeBool(e.boolean); return true;
        case Expr::Kind::String: out = MakeString(e.text); return true;
        case Expr::Kind::Var: {
            if (!GetVarValue(localVars, rt, e.text, out)) {
                err = "Unknown variable '" + e.text + "'";
                return false;
            }
            return true;
        }
        case Expr::Kind::Path:
            return EvalPathExpr(e.pathSegs, localVars, self, rt, out, err);
        case Expr::Kind::Tuple: {
            if (e.items.size() != 3) { err = "A tuple must have exactly three values"; return false; }
            float vals[3];
            for (int i = 0; i < 3; ++i) {
                Value v;
                if (!EvalExpr(e.items[i], localVars, self, rt, v, err)) return false;
                if (v.type != Value::Type::Number) { err = "Tuple values must be numbers"; return false; }
                vals[i] = v.number;
            }
            out = MakeTuple(vals[0], vals[1], vals[2]);
            return true;
        }
        case Expr::Kind::Neg: {
            if (e.items.size() != 1) { err = "Internal error: invalid unary expression"; return false; }
            Value v;
            if (!EvalExpr(e.items[0], localVars, self, rt, v, err)) return false;
            if (v.type != Value::Type::Number) { err = "Cannot negate a non-number"; return false; }
            out = MakeNumber(-v.number);
            return true;
        }
        case Expr::Kind::Not: {
            if (e.items.size() != 1) { err = "Internal error: invalid 'not' expression"; return false; }
            Value v;
            if (!EvalExpr(e.items[0], localVars, self, rt, v, err)) return false;
            if (v.type != Value::Type::Bool) { err = "'not' needs a true/false value"; return false; }
            out = MakeBool(!v.boolean);
            return true;
        }
        case Expr::Kind::Binary: {
            if (e.items.size() != 2) { err = "Internal error: invalid binary expression"; return false; }
            Value l, r;
            if (!EvalExpr(e.items[0], localVars, self, rt, l, err)) return false;
            if (!EvalExpr(e.items[1], localVars, self, rt, r, err)) return false;
            if (e.op == "==" || e.op == "!=") {
                bool eq = false;
                if (!ValuesEqual(l, r, eq, err)) return false;
                out = MakeBool(e.op == "==" ? eq : !eq);
                return true;
            }
            if (e.op == "and" || e.op == "or") {
                if (l.type != Value::Type::Bool || r.type != Value::Type::Bool) {
                    err = "Cannot use '" + e.op + "' with a non-boolean value";
                    return false;
                }
                out = MakeBool(e.op == "and" ? (l.boolean && r.boolean) : (l.boolean || r.boolean));
                return true;
            }
            if (l.type != Value::Type::Number || r.type != Value::Type::Number) {
                err = "Arithmetic ('" + e.op + "') only works on numbers";
                return false;
            }
            float a = l.number;
            float b = r.number;
            float res = 0.0f;
            if (e.op == "+")      { res = a + b; }
            else if (e.op == "-") { res = a - b; }
            else if (e.op == "*") { res = a * b; }
            else if (e.op == "/") { if (b == 0.0f) { err = "Division by zero"; return false; } res = a / b; }
            else { err = "Unknown operator '" + e.op + "'"; return false; }
            out = MakeNumber(res);
            return true;
        }
        case Expr::Kind::Call: {
            std::string callee = e.text;
            if (callee == "Vector3.new") {
                if (e.items.size() != 3) {
                    err = "Vector3.new() needs exactly 3 numbers (Vector3.new(x, y, z))";
                    return false;
                }
                float vals[3];
                for (int i = 0; i < 3; ++i) {
                    Value v;
                    if (!EvalExpr(e.items[i], localVars, self, rt, v, err)) return false;
                    if (v.type != Value::Type::Number) { err = "Vector3.new() arguments must be numbers"; return false; }
                    vals[i] = v.number;
                }
                out = MakeTuple(vals[0], vals[1], vals[2]);
                return true;
            }
            if (callee == "rgb") {
                if (e.items.size() != 3 && e.items.size() != 5) {
                    err = "rgb() needs 3 numbers (rgb(r, g, b)) or 5 (rgb(r, g, b, sat, brightness))";
                    return false;
                }
                float vals[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                for (size_t i = 0; i < e.items.size(); ++i) {
                    Value v;
                    if (!EvalExpr(e.items[i], localVars, self, rt, v, err)) return false;
                    if (v.type != Value::Type::Number) { err = "rgb() arguments must be numbers"; return false; }
                    vals[i] = v.number;
                }
                out = MakeColor(MakeRgbColor(vals[0], vals[1], vals[2], e.items.size() == 5, vals[3], vals[4]));
                return true;
            }
            if (callee == "Instance.new") {
                if (e.items.size() != 1) {
                    err = "Instance.new() needs exactly 1 string argument: Instance.new(\"Cube\")";
                    return false;
                }
                Value arg;
                if (!EvalExpr(e.items[0], localVars, self, rt, arg, err)) return false;
                if (arg.type != Value::Type::String) {
                    err = "Instance.new() argument must be a string (\"Cube\", \"Sphere\", \"Cylinder\", \"Wedge\")";
                    return false;
                }
                ScatteredObject* obj = rt.CreateObject(arg.str);
                if (!obj) {
                    err = "Unknown shape '" + arg.str + "'. Use Cube, Sphere, Cylinder, or Wedge.";
                    return false;
                }
                out = MakeObject(obj);
                return true;
            }
            err = "Unknown function '" + callee + "'";
            return false;
        }
    }
    err = "Unknown expression";
    return false;
}

bool ExpandObjectVarPath(std::string& path, std::unordered_map<std::string, Value>& localVars,
                         Runtime& rt, std::string& err, ScatteredObject*& resolvedObj) {
    resolvedObj = nullptr;
    size_t dot = path.find('.');
    std::string first = dot == std::string::npos ? path : path.substr(0, dot);
    std::string rest = dot == std::string::npos ? "" : path.substr(dot + 1);
    if (first == "self" || first == "game") return true;
    Value v;
    if (!GetVarValue(localVars, rt, first, v)) {
        err = "Unknown variable '" + first + "'";
        return false;
    }
    if (v.type != Value::Type::Object) {
        err = "Variable '" + first + "' is not an object";
        return false;
    }
    const auto& objs = rt.GetObjects();
    if (!v.object || std::find(objs.begin(), objs.end(), v.object) == objs.end()) {
        err = "Object referenced by '" + first + "' no longer exists";
        return false;
    }
    resolvedObj = v.object;
    path = "game.Workspace." + v.object->GetName() + (rest.empty() ? "" : "." + rest);
    return true;
}

bool ExpandObjectVarPath(std::string& path, std::unordered_map<std::string, Value>& localVars,
                         Runtime& rt, std::string& err) {
    ScatteredObject* dummy = nullptr;
    return ExpandObjectVarPath(path, localVars, rt, err, dummy);
}

}

Runtime::Runtime(std::vector<ScatteredObject*>& objects,
                 std::vector<std::unique_ptr<ModelGroup>>& models,
                 Camera3D& camera, phys::Simulation& sim, Engine& engine)
    : objects(objects), models(models), camera(camera), sim(sim), engine(engine) {}

Runtime::~Runtime() = default;

bool Runtime::StartScript(ScatteredObject* self, const std::string& source,
                          int& outErrLine, std::string& outErrMsg) {
    StopScript(self);
    auto coro = std::make_unique<Coroutine>();
    coro->program = Compile(source);
    if (!coro->program.ok()) {
        outErrLine = coro->program.errorLine;
        outErrMsg = coro->program.error;
        ui::Log("Script error (line %d): %s", outErrLine, outErrMsg.c_str());
        return false;
    }
    coro->ownerObject = self;
    coro->self = self;
    Coroutine::Frame top;
    top.body = &coro->program.body;
    top.isProgram = true;
    coro->stack.push_back(std::move(top));
    coroutines.push_back(std::move(coro));
    std::string owner = self ? self->GetName() : std::string("?");
    ui::Log("Started script on '%s'", owner.c_str());
    if (!sandboxActive) BeginSandbox();
    return true;
}

bool Runtime::StartScript(int index, int& outErrLine, std::string& outErrMsg) {
    if (index < 0 || index >= (int)scripts.size()) {
        outErrMsg = "Invalid script index";
        return false;
    }
    StopScript(index);
    auto coro = std::make_unique<Coroutine>();
    coro->program = Compile(scripts[index].source);
    if (!coro->program.ok()) {
        outErrLine = coro->program.errorLine;
        outErrMsg = coro->program.error;
        ui::Log("Script error (line %d): %s", outErrLine, outErrMsg.c_str());
        return false;
    }
    coro->ownerIndex = index;
    Coroutine::Frame top;
    top.body = &coro->program.body;
    top.isProgram = true;
    coro->stack.push_back(std::move(top));
    coroutines.push_back(std::move(coro));
    ui::Log("Started script '%s'", scripts[index].name.c_str());
    if (!sandboxActive) BeginSandbox();
    return true;
}

void Runtime::StopScript(ScatteredObject* self) {
    coroutines.erase(std::remove_if(coroutines.begin(), coroutines.end(),
        [self](const std::unique_ptr<Coroutine>& c) { return c->ownerObject == self; }),
        coroutines.end());
}

void Runtime::StopScript(int index) {
    coroutines.erase(std::remove_if(coroutines.begin(), coroutines.end(),
        [index](const std::unique_ptr<Coroutine>& c) { return c->ownerIndex == index; }),
        coroutines.end());
}

void Runtime::RemoveScript(int index) {
    if (index < 0 || index >= (int)scripts.size()) return;
    StopScript(index);
    scripts.erase(scripts.begin() + index);
    for (auto& c : coroutines) {
        if (c->ownerIndex > index) --c->ownerIndex;
    }
}

bool Runtime::IsRunning(ScatteredObject* self) const {
    for (const auto& c : coroutines)
        if (c->ownerObject == self && !c->done && !c->error) return true;
    return false;
}

bool Runtime::IsRunning(int index) const {
    for (const auto& c : coroutines)
        if (c->ownerIndex == index && !c->done && !c->error) return true;
    return false;
}

int Runtime::GetErrorLine(ScatteredObject* self) const {
    for (const auto& c : coroutines)
        if (c->ownerObject == self) return c->errLine;
    return 0;
}

const std::string& Runtime::GetErrorMessage(ScatteredObject* self) const {
    static std::string empty;
    for (const auto& c : coroutines)
        if (c->ownerObject == self) return c->errMsg;
    return empty;
}

int Runtime::GetErrorLine(int index) const {
    for (const auto& c : coroutines)
        if (c->ownerIndex == index) return c->errLine;
    return 0;
}

const std::string& Runtime::GetErrorMessage(int index) const {
    static std::string empty;
    for (const auto& c : coroutines)
        if (c->ownerIndex == index) return c->errMsg;
    return empty;
}

void Runtime::StopAll() {
    coroutines.clear();
}

ScatteredObject* Runtime::FindByName(const std::string& name) const {
    for (auto* obj : objects)
        if (obj && obj->GetName() == name) return obj;
    return nullptr;
}

void Runtime::SetBodyVelocity(ScatteredObject* object, Vector3 velocity) {
    sim.SetBodyVelocity(object, velocity);
}

void Runtime::SetPhysicsGravity(float g) { sim.SetGravity(g); }
float Runtime::GetPhysicsGravity() const { return sim.GetGravity(); }
void Runtime::SetPhysicsFriction(float f) { sim.SetFriction(f); }
float Runtime::GetPhysicsFriction() const { return sim.GetFriction(); }
void Runtime::SetPhysicsRestitution(float r) { sim.SetRestitution(r); }
float Runtime::GetPhysicsRestitution() const { return sim.GetRestitution(); }

void Runtime::SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity) {
    sim.SetBodyAngularVelocity(object, velocity);
}

void Runtime::SetBodyPosition(ScatteredObject* object, Vector3 position) {
    sim.SetBodyPosition(object, position);
}

void Runtime::SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg) {
    sim.SetBodyOrientation(object, eulerDeg);
}

ScatteredObject* Runtime::CreateObject(const std::string& shapeName) {
    ShapeType type = ShapeType::Cube;
    if (shapeName == "Sphere")   type = ShapeType::Sphere;
    else if (shapeName == "Cylinder") type = ShapeType::Cylinder;
    else if (shapeName == "Wedge")    type = ShapeType::Wedge;
    else if (shapeName != "Cube") return nullptr;

    auto newObj = std::make_unique<ScatteredObject>(
        Vector3{ 0, 0, 0 }, Vector3{ 1.5f, 1.5f, 1.5f }, SKYBLUE, type);
    newObj->SetName(shapeName);
    newObj->anchored = false;
    ScatteredObject* raw = newObj.get();
    objects.push_back(raw);
    engine.AddEntity(std::move(newObj));
    if (isPlaying) {
        playCreatedObjects.push_back(raw);
        sim.SpawnBodyForObject(raw);
    }
    return raw;
}

void Runtime::LogPropertyChange(ScatteredObject* obj, const std::string& prop,
                                const std::string& newValue) {
    if (!isPlaying) return;
    for (const auto& c : playChanges)
        if (c.obj == obj && c.prop == prop) return;
    std::string oldValue;
    if (prop == "Position") {
        Vector3 v = *obj->GetPosPtr();
        oldValue = "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
    } else if (prop == "Rotation") {
        Vector3 v = *obj->GetRotationPtr();
        oldValue = "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
    } else if (prop == "Size") {
        Vector3 v = *obj->GetSizePtr();
        oldValue = "(" + std::to_string(v.x) + ", " + std::to_string(v.y) + ", " + std::to_string(v.z) + ")";
    } else if (prop == "Anchored") {
        oldValue = obj->anchored ? "true" : "false";
    } else if (prop == "CanCollide") {
        oldValue = obj->canCollide ? "true" : "false";
    } else if (prop == "Color") {
        Color c = *obj->GetColorPtr();
        oldValue = "(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ")";
    } else if (prop == "Mass") {
        oldValue = std::to_string(obj->GetMass());
    } else {
        return;
    }
    playChanges.push_back({ obj, prop, oldValue });
}

void Runtime::RollbackPlayChanges() {
    ui::SetSelection({}, nullptr);
    for (auto it = playChanges.rbegin(); it != playChanges.rend(); ++it) {
        ScatteredObject* obj = it->obj;
        if (!obj) continue;
        bool created = std::find(playCreatedObjects.begin(), playCreatedObjects.end(), obj)
                       != playCreatedObjects.end();
        if (created) continue;
        const std::string& prop = it->prop;
        const std::string& val = it->oldValue;
        std::string err;
        SetObjectProperty(obj, prop, val, *this, err);
    }
    playChanges.clear();

    for (auto it = playCreatedObjects.rbegin(); it != playCreatedObjects.rend(); ++it) {
        ScatteredObject* obj = *it;
        if (!obj) continue;
        auto fit = std::find(objects.begin(), objects.end(), obj);
        if (fit != objects.end()) objects.erase(fit);
        engine.RemoveEntity(obj);
    }
    playCreatedObjects.clear();
}

void Runtime::OnPlayStarted() {
    playCreatedObjects.clear();
    playChanges.clear();
    for (auto* obj : objects) {
        if (!obj || !obj->runOnPlay || obj->script.empty()) continue;
        int line = 0;
        std::string msg;
        StartScript(obj, obj->script, line, msg);
    }
    for (size_t i = 0; i < scripts.size(); ++i) {
        if (!scripts[i].runOnPlay || scripts[i].source.empty()) continue;
        int line = 0;
        std::string msg;
        StartScript((int)i, line, msg);
    }
}

void Runtime::OnPlayStopped() {
    RollbackPlayChanges();
    StopAll();
}

void Runtime::RunCoroutine(Coroutine& c, double now, int budget) {
    for (int steps = 0; steps < budget; ++steps) {
        if (c.done || c.error) return;
        Coroutine::Frame& f = c.stack.back();

        if (f.pc >= f.body->size()) {
            if (f.isProgram) { c.done = true; return; }
            if (f.kind == BlockKind::Every && f.instanceIdx + 1 < f.instances.size()) {
                f.instanceIdx++;
                c.self = f.instances[f.instanceIdx];
                f.pc = 0;
                continue;
            }
            if (f.kind == BlockKind::Count && f.count > 1) {
                f.count--;
                f.pc = 0;
                continue;
            }
            if (f.kind == BlockKind::While) {
                std::string err;
                Value cond;
                if (!EvalExpr(*f.condition, c.vars, c.self, *this, cond, err) ||
                    cond.type != Value::Type::Bool) {
                    if (err.empty()) err = "While condition must be true or false";
                    c.error = true;
                    c.errLine = f.condition->line;
                    c.errMsg = err;
                    ui::Log("Script error (line %d): %s", c.errLine, err.c_str());
                    return;
                }
                if (cond.boolean) { f.pc = 0; continue; }
            }
            c.self = f.outerSelf;
            c.stack.pop_back();
            continue;
        }

        const Stmt& s = (*f.body)[f.pc];
        f.pc++;

        switch (s.type) {
            case Stmt::Type::Assign: {
                std::string err;
                Value v;
                if (!EvalExpr(s.value, c.vars, c.self, *this, v, err)) {
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                std::string rhs;
                if (!ValueToAssignmentString(v, rhs)) {
                    err = "Cannot assign an object to a property";
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                std::string path = s.path;
                ScatteredObject* resolvedObj = nullptr;
                if (!ExpandObjectVarPath(path, c.vars, *this, err, resolvedObj)) {
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                if (resolvedObj) {
                    std::string actualProp = s.property;
                    size_t lastDot = path.rfind('.');
                    if (lastDot != std::string::npos) actualProp = path.substr(lastDot + 1);
                    if (!SetObjectProperty(resolvedObj, actualProp, rhs, *this, err)) {
                        c.error = true; c.errLine = s.line; c.errMsg = err;
                        ui::Log("Script error (line %d): %s", s.line, err.c_str());
                        return;
                    }
                } else if (!ExecuteAssignment(path, s.property, rhs, c.self, *this, err)) {
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                break;
            }
            case Stmt::Type::VarDecl: {
                std::string err;
                Value v;
                if (!EvalExpr(s.value, c.vars, c.self, *this, v, err)) {
                    c.error = true; c.errLine = s.line;
                    c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                if (s.isDeclaration) {
                    if (s.isGlobal) this->globalVars[s.varName] = v;
                    else c.vars[s.varName] = v;
                } else if (c.vars.find(s.varName) != c.vars.end()) {
                    c.vars[s.varName] = v;
                } else if (this->globalVars.find(s.varName) != this->globalVars.end()) {
                    this->globalVars[s.varName] = v;
                } else {
                    err = "Unknown variable '" + s.varName + "'";
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                break;
            }
            case Stmt::Type::Wait: {
                if (s.seconds <= 0.0f) c.runNextFrame = true;
                else c.waitUntil = now + s.seconds;
                return;
            }
            case Stmt::Type::Block: {
                if (s.kind == BlockKind::If || s.kind == BlockKind::While) {
                    std::string err;
                    Value cond;
                    if (!EvalExpr(s.condition, c.vars, c.self, *this, cond, err)) {
                        c.error = true; c.errLine = s.line; c.errMsg = err;
                        ui::Log("Script error (line %d): %s", s.line, err.c_str());
                        return;
                    }
                    if (cond.type != Value::Type::Bool) {
                        err = s.kind == BlockKind::If
                                  ? "If condition must be true or false"
                                                      : "While condition must be true or false";
                        c.error = true; c.errLine = s.line; c.errMsg = err;
                        ui::Log("Script error (line %d): %s", s.line, err.c_str());
                        return;
                    }
                    if (s.kind == BlockKind::If) {
                        if (!cond.boolean && s.elseBody.empty()) break;
                        Coroutine::Frame nf;
                        nf.body = cond.boolean ? &s.body : &s.elseBody;
                        nf.kind = BlockKind::If;
                        nf.condition = &s.condition;
                        nf.outerSelf = c.self;
                        c.stack.push_back(std::move(nf));
                    } else {
                        if (!cond.boolean) break;
                        Coroutine::Frame nf;
                        nf.body = &s.body;
                        nf.kind = BlockKind::While;
                        nf.condition = &s.condition;
                        nf.outerSelf = c.self;
                        c.stack.push_back(std::move(nf));
                    }
                    break;
                }
                Coroutine::Frame nf;
                nf.body = &s.body;
                nf.kind = s.kind;
                nf.count = s.count;
                nf.outerSelf = c.self;
                if (s.kind == BlockKind::Every) {
                    for (auto* o : objects) if (o) nf.instances.push_back(o);
                    if (nf.instances.empty()) break;
                    nf.instanceIdx = 0;
                    c.self = nf.instances[0];
                } else if (s.kind == BlockKind::Count && s.count <= 0) {
                    break;
                }
                c.stack.push_back(std::move(nf));
                break;
            }
            case Stmt::Type::Print: {
                std::string err;
                Value v;
                if (!EvalExpr(s.value, c.vars, c.self, *this, v, err)) {
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                ui::Log("%s", FormatValue(v).c_str());
                break;
            }
            case Stmt::Type::ExprStmt: {
                std::string err;
                Value v;
                if (!EvalExpr(s.value, c.vars, c.self, *this, v, err)) {
                    c.error = true; c.errLine = s.line; c.errMsg = err;
                    ui::Log("Script error (line %d): %s", s.line, err.c_str());
                    return;
                }
                break;
            }
        }
    }
    if (!c.done && !c.error) {
        c.error = true;
        c.errMsg = "Script ran too long without tick.wait()";
        if (!c.stack.empty()) {
            const Coroutine::Frame& f = c.stack.back();
            if (f.pc < f.body->size()) c.errLine = (*f.body)[f.pc].line;
        }
    }
}

void Runtime::StepCoroutines() {
    const double now = GetTime();
    for (auto& c : coroutines) {
        if (c->done || c->error) continue;
        if (c->runNextFrame) {
            c->runNextFrame = false;
        } else if (c->waitUntil > now) {
            continue;
        }
        RunCoroutine(*c, now, RUN_BUDGET);
    }
}

void Runtime::BeginSandbox() {
    snapshot.objects.clear();
    for (auto* obj : objects) {
        if (!obj) continue;
        snapshot.objects.push_back({ obj, *obj->GetPosPtr(), *obj->GetSizePtr(),
                                     *obj->GetRotationPtr(), obj->GetVelocity(), obj->GetAngularVelocity(),
                                     *obj->GetColorPtr(), obj->anchored, obj->GetStoredMass(),
                                     obj->canCollide, obj->GetCollisionAccuracy() });
    }
    snapshot.shadows = gfx::IsShadowsEnabled();
    snapshot.grid = gfx::IsGridVisible();
    snapshot.wireframe = gfx::IsWireframe();
    snapshot.ambient = gfx::GetAmbientIntensity();
    snapshot.shadowQuality = gfx::GetShadowQuality();
    snapshot.fov = camera.fovy;
    snapshot.physicsGravity = sim.GetGravity();
    snapshot.physicsFriction = sim.GetFriction();
    snapshot.physicsRestitution = sim.GetRestitution();
    sandboxActive = true;
}

void Runtime::EndSandbox() {
    for (const auto& s : snapshot.objects) {
        if (!s.obj || std::find(objects.begin(), objects.end(), s.obj) == objects.end()) continue;
        *s.obj->GetPosPtr() = s.pos;
        *s.obj->GetSizePtr() = s.size;
        *s.obj->GetRotationPtr() = s.rot;
        s.obj->SetVelocity(s.velocity);
        s.obj->SetAngularVelocity(s.angularVelocity);
        *s.obj->GetColorPtr() = s.color;
        s.obj->anchored = s.anchored;
        s.obj->canCollide = s.canCollide;
        s.obj->SetCollisionAccuracy(s.accuracy);
        if (s.mass <= 0.0f) s.obj->ResetMassAuto();
        else s.obj->SetMass(s.mass);
    }
    gfx::SetShadowsEnabled(snapshot.shadows);
    gfx::SetGridVisible(snapshot.grid);
    gfx::SetWireframe(snapshot.wireframe);
    gfx::SetAmbientIntensity(snapshot.ambient);
    gfx::SetShadowQuality(snapshot.shadowQuality);
    camera.fovy = snapshot.fov;
    sim.SetGravity(snapshot.physicsGravity);
    sim.SetFriction(snapshot.physicsFriction);
    sim.SetRestitution(snapshot.physicsRestitution);
    sandboxActive = false;
}

void Runtime::EndSandboxIfIdle() {
    if (isPlaying) return;
    if (sandboxActive && coroutines.empty()) EndSandbox();
}

void Runtime::Update(float) {
    bool playing = ui::IsPlayActive();
    isPlaying = playing;
    if (playing && !wasPlaying) OnPlayStarted();
    if (!playing && wasPlaying) OnPlayStopped();
    wasPlaying = playing;

    StepCoroutines();

    for (auto it = coroutines.begin(); it != coroutines.end();) {
        if ((*it)->done) it = coroutines.erase(it);
        else ++it;
    }

    EndSandboxIfIdle();
}

Runtime* g_runtime = nullptr;

Runtime& GetRuntime() { return *g_runtime; }
Runtime* GetRuntimePtr() { return g_runtime; }
bool IsRuntimeReady() { return g_runtime != nullptr; }
void SetRuntime(Runtime* runtime) { g_runtime = runtime; }

}