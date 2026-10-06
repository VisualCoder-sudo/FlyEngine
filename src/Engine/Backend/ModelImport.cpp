#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "ufbx.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string Lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string ToForwardSlashes(std::string s) {
    for (auto& c : s) if (c == '\\') c = '/';
    return s;
}

std::string FileExtension(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return "";
    return Lowercase(p.substr(dot));
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::string ReplaceLast(std::string line, const std::string& needle, const std::string& repl) {
    size_t pos = line.rfind(needle);
    if (pos == std::string::npos) return line;
    return line.replace(pos, needle.size(), repl);
}

std::string ReplaceAll(std::string s, const std::string& needle, const std::string& repl) {
    if (needle.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), repl);
        pos += repl.size();
    }
    return s;
}

std::string ReadTextFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool WriteTextFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

bool CopyFile(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

std::vector<std::string> Tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

bool IsNumericToken(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i++;
    bool hasDigit = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (std::isdigit(static_cast<unsigned char>(c))) { hasDigit = true; continue; }
        if (c == '.' || c == 'e' || c == 'E') continue;
        return false;
    }
    return hasDigit;
}

bool IsTextureKeyword(const std::string& tok) {
    if (StartsWith(tok, "map_")) return true;
    return tok == "bump" || tok == "disp" || tok == "refl";
}

std::string LastTextureToken(const std::vector<std::string>& tokens) {
    for (auto it = tokens.rbegin(); it != tokens.rend(); ++it) {
        if (!IsNumericToken(*it) && !StartsWith(*it, "-")) return *it;
    }
    return {};
}

void CopyMtlWithDeps(const fs::path& srcMtl, const fs::path& targetDir,
                     std::vector<std::string>& missing) {
    std::string text = ReadTextFile(srcMtl);
    if (text.empty()) {
        missing.push_back(srcMtl.filename().string());
        return;
    }
    const fs::path srcDir = srcMtl.parent_path();
    std::istringstream lines(text);
    std::string line;
    std::ostringstream out;
    while (std::getline(lines, line)) {
        std::string rewritten = line;
        std::vector<std::string> tok = Tokenize(line);
        if (!tok.empty() && IsTextureKeyword(tok[0]) && tok.size() >= 2) {
            std::string texName = LastTextureToken(tok);
            if (!texName.empty()) {
                std::string base = fs::path(texName).filename().string();
                fs::path srcTex = srcDir / texName;
                if (fs::exists(srcTex)) {
                    CopyFile(srcTex, targetDir / base);
                } else {
                    missing.push_back(texName);
                }
                rewritten = ReplaceLast(rewritten, texName, base);
            }
        }
        out << rewritten << '\n';
    }
    CopyFile(srcMtl, targetDir / srcMtl.filename());
    WriteTextFile(targetDir / srcMtl.filename(), out.str());
}

void CopyObjWithDeps(const fs::path& src, const fs::path& targetDir,
                     std::vector<std::string>& missing) {
    const fs::path srcDir = src.parent_path();
    std::string text = ReadTextFile(src);
    std::istringstream lines(text);
    std::string line;
    std::ostringstream out;
    while (std::getline(lines, line)) {
        std::string rewritten = line;
        std::vector<std::string> tok = Tokenize(line);
        if (!tok.empty() && tok[0] == "mtllib" && tok.size() >= 2) {
            for (size_t t = 1; t < tok.size(); ++t) {
                std::string mtlName = tok[t];
                std::string base = fs::path(mtlName).filename().string();
                fs::path srcMtl = srcDir / mtlName;
                if (fs::exists(srcMtl)) {
                    CopyMtlWithDeps(srcMtl, targetDir, missing);
                } else {
                    missing.push_back(mtlName);
                }
                rewritten = ReplaceLast(rewritten, mtlName, base);
            }
        }
        out << rewritten << '\n';
    }
    CopyFile(src, targetDir / src.filename());
    WriteTextFile(targetDir / src.filename(), out.str());
}

void CopyGltfWithDeps(const fs::path& src, const fs::path& targetDir,
                      std::vector<std::string>& missing) {    std::string text = ReadTextFile(src);
    if (text.empty()) {
        missing.push_back(src.filename().string());
        return;
    }
    const fs::path srcDir = src.parent_path();
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t key = text.find("\"uri\"", pos);
        if (key == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, key - pos);
        size_t colon = text.find(':', key + 5);
        if (colon == std::string::npos) {
            out.append(text, key, std::string::npos);
            break;
        }
        size_t q = text.find('"', colon + 1);
        if (q == std::string::npos) {
            out.append(text, key, std::string::npos);
            break;
        }
        size_t q2 = text.find('"', q + 1);
        if (q2 == std::string::npos) {
            out.append(text, key, std::string::npos);
            break;
        }
        out.append(text, key, q - key + 1); // up to and including the opening quote
        std::string uri = text.substr(q + 1, q2 - q - 1);
        if (StartsWith(uri, "data:")) {
            out.append(uri);
        } else {
            std::string base = fs::path(uri).filename().string();
            if (base.empty() || base == uri) {
                out.append(uri);
            } else {
                fs::path srcFile = srcDir / uri;
                if (fs::exists(srcFile)) {
                    CopyFile(srcFile, targetDir / base);
                } else {
                    missing.push_back(uri);
                }
                out.append(base);
            }
        }
        pos = q2 + 1; // closing quote handled next iteration
        out.append(1, '"');
    }
    CopyFile(src, targetDir / src.filename());
    WriteTextFile(targetDir / src.filename(), out);
}

// Copy an FBX file and any externally-referenced texture files referenced by
// its materials so the imported model is self-contained under assets/3D/.
// Embedded textures (stored inside the .fbx) are written out to files too.
void CopyFbxWithDeps(const fs::path& src, const fs::path& targetDir,
                      std::vector<std::string>& missing) {
    const fs::path srcDir = src.parent_path();

    ufbx_load_opts opts = { };
    opts.load_external_files = true;
    opts.ignore_missing_external_files = true;
    opts.ignore_geometry = true;
    opts.ignore_animation = true;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(src.string().c_str(), &opts, &error);
    if (scene) {
        for (size_t i = 0; i < scene->textures.count; ++i) {
            const ufbx_texture* tex = scene->textures.data[i];
            if (!tex) continue;
            if (tex->content.size > 0) {
                // Embedded texture: write raw bytes to a file in targetDir.
                std::string ext = ".png";
                if (tex->filename.length) {
                    fs::path fn(tex->filename.data);
                    if (!fn.extension().empty()) ext = fn.extension().string();
                }
                fs::path base = tex->filename.length ? fs::path(tex->filename.data).filename().string()
                                                     : (std::string("texture_") + std::to_string(i) + ext);
                fs::path dst = targetDir / base;
                std::ofstream out(dst, std::ios::binary | std::ios::trunc);
                if (out) {
                    out.write((const char*)tex->content.data, (std::streamsize)tex->content.size);
                    out.close();
                } else {
                    missing.push_back(base.string());
                }
            } else if (tex->filename.length > 0) {
                fs::path rel(tex->filename.data);
                fs::path srcTex = srcDir / rel;
                if (fs::exists(srcTex)) {
                    CopyFile(srcTex, targetDir / rel.filename());
                } else {
                    missing.push_back(rel.filename().string());
                }
            }
        }
        ufbx_free_scene(scene);
    }

    CopyFile(src, targetDir / src.filename());
}

std::string FormatMissing(const std::vector<std::string> &missing, const std::string& modelName) {
    std::ostringstream m;
    m << "missing " << missing.size() << " dependency file(s) for " << modelName;
    for (size_t i = 0; i < missing.size() && i < 3; ++i) m << ": " << missing[i];
    if (missing.size() > 3) m << " (+" << (missing.size() - 3) << " more)";
    return m.str();
}

}

const std::vector<platform::FileFilter>& ModelFilters() {
    static const std::vector<platform::FileFilter> filters = {
        {"3D Models", "*.obj;*.gltf;*.glb;*.fbx;*.iqm;*.vox;*.m3d"},
        {"OBJ", "*.obj"},
        {"glTF", "*.gltf;*.glb"},
        {"FBX", "*.fbx"},
        {"All Files", "*"},
    };
    return filters;
}

const std::vector<platform::FileFilter>& TextureFilters() {
    static const std::vector<platform::FileFilter> filters = {
        {"Images", "*.png;*.jpg;*.bmp;*.tga;*.webp"},
        {"All Files", "*"},
    };
    return filters;
}

bool BeginChooseModelOpenPath(const std::string& startDir) {
    return platform::BeginOpenFileDialog(platform::DialogPurpose::ImportModel, "Import 3D Model",
                                         startDir, ModelFilters());
}

bool BeginChooseTexturePath(const std::string& startDir) {
    return platform::BeginOpenFileDialog(platform::DialogPurpose::ImportTexture, "Import Texture",
                                         startDir, TextureFilters());
}

ImportResult ImportModel(const std::string& sourcePath,
                         const std::string& projectDir,
                         const std::string& projectName,
                         const std::string& targetParentDir) {
    ImportResult result;
    if (sourcePath.empty() || projectDir.empty()) {
        result.error = "No project is open to import into.";
        return result;
    }

    fs::path src(sourcePath);
    std::error_code ec;
    if (!fs::exists(src, ec)) {
        result.error = "File not found: " + sourcePath;
        return result;
    }

    const std::string ext = FileExtension(sourcePath);
    if (ext != ".obj" && ext != ".gltf" && ext != ".glb" && ext != ".iqm" &&
        ext != ".vox" && ext != ".m3d" && ext != ".fbx") {
        result.error = "Unsupported model type: " + ext;
        return result;
    }

    fs::path base(projectDir);
    fs::path assetsRoot = targetParentDir.empty() ? (base / "assets" / "3D") : fs::path(targetParentDir);
    fs::create_directories(assetsRoot, ec);
    if (ec) {
        result.error = "Could not create " + assetsRoot.string();
        return result;
    }

    std::string stem = src.stem().string();
    fs::path targetDir = assetsRoot / stem;
    int n = 1;
    while (fs::exists(targetDir, ec)) {
        targetDir = assetsRoot / (stem + "_" + std::to_string(n++));
    }
    fs::create_directories(targetDir, ec);
    if (ec) {
        result.error = "Could not create " + targetDir.string();
        return result;
    }

    std::vector<std::string> missing;
    if (ext == ".obj") CopyObjWithDeps(src, targetDir, missing);
    else if (ext == ".gltf") CopyGltfWithDeps(src, targetDir, missing);
    else if (ext == ".fbx") CopyFbxWithDeps(src, targetDir, missing);
    else {
        fs::path dst = targetDir / src.filename();
        if (!CopyFile(src, dst)) {
            result.error = "Failed to copy " + src.string();
            return result;
        }
    }

    fs::path copiedModel = targetDir / src.filename();
    if (!fs::exists(copiedModel, ec)) {
        result.error = "Model copy was not created: " + copiedModel.string();
        return result;
    }

    fs::path stored = fs::relative(copiedModel, base, ec);
    result.storedPath = (!ec && !stored.empty()) ? ToForwardSlashes(stored.generic_string())
                                                 : copiedModel.generic_string();
    result.targetDir = targetDir.lexically_normal().string();
    result.warning = FormatMissing(missing, src.filename().string());
    result.ok = true;
    return result;
}

std::string ResolveStoredAssetPath(const std::string& stored, const std::string& projectDir) {
    if (stored.empty() || projectDir.empty()) return stored;
    if (IsAbsolutePath(stored)) return stored;
    std::error_code ec;
    fs::path resolved = fs::absolute(fs::path(projectDir) / stored, ec);
    if (ec) return stored;
    return resolved.lexically_normal().string();
}

bool IsAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    // std::filesystem handles every case correctly per-platform: "C:\x" and
    // "\\server\share" on Windows, a leading "/" on POSIX. The previous
    // hand-rolled check only recognised a drive letter or a leading "//", so a
    // normal POSIX path like "/home/u/proj/a.png" was treated as *relative* and
    // re-resolved against the project directory.
    // is_absolute() is a pure query over the path grammar: it cannot
    // touch the filesystem, so there is no error_code overload and no
    // reason to want one.
    return fs::path(fs::u8path(p)).is_absolute();
}

std::string PathRelativeTo(const std::string& p, const std::string& base) {
    if (p.empty() || base.empty()) return p;
    std::error_code ec;
    fs::path absP = fs::absolute(p, ec);
    if (ec) return p;
    fs::path absB = fs::absolute(base, ec);
    if (ec) return p;
    fs::path rel = fs::relative(absP, absB, ec);
    if (ec) return p;
    if (rel.empty()) return p;
    for (const auto& part : rel) {
        if (part == "..") return p;
    }
    return ToForwardSlashes(rel.generic_string());
}