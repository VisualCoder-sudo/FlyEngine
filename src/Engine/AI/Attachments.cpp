// Attachments.cpp: reading a file or an image the user adds with the + menu into something a request can carry.
#include "../../../include/Engine/AI/AI.hpp"
#include "../../../include/Engine/U8Path.hpp"
#include "raylib.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace ai {

std::string Base64Encode(const std::string& in) {
    static const char* const kTable = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        out += kTable[(v >> 18) & 63]; out += kTable[(v >> 12) & 63]; out += kTable[(v >> 6) & 63]; out += kTable[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = (unsigned char)in[i] << 16;
        out += kTable[(v >> 18) & 63]; out += kTable[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        out += kTable[(v >> 18) & 63]; out += kTable[(v >> 12) & 63]; out += kTable[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string ComposeUserText(const std::string& typed, const std::vector<Attachment>& attachments) {
    std::string out = typed;
    for (const Attachment& a : attachments) {
        if (a.image) {
            if (a.data.empty()) out += "\n\n[An image named " + a.name + " was attached here but is no longer available.]";
            continue;
        }
        out += "\n\n[Attached file: " + a.name + "]\n```\n" + a.data + (a.data.empty() || a.data.back() != '\n' ? "\n" : "") + "```";
    }
    return out;
}

namespace {
namespace fs = std::filesystem;

std::string LowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool ReadFileBytes(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool LoadImageAttachment(const fs::path& file, const std::string& path, Attachment& out, std::string& error) {
    Image img = LoadImage(path.c_str());
    if (img.data == nullptr) { error = "Could not read that as an image (png, jpg, bmp, gif, tga or psd)."; return false; }
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);

    const std::string ext = LowerExt(file);
    std::string original;
    const bool plainSource = (ext == ".png" || ext == ".jpg" || ext == ".jpeg") && ReadFileBytes(file, original);
    const int maxSide = std::max(img.width, img.height);
    if (plainSource && original.size() <= 4u * 1024 * 1024 && maxSide <= kMaxImageSide) {
        out.data = Base64Encode(original);               // already a good size: send the file as it is
        out.mime = ext == ".png" ? "image/png" : "image/jpeg";
        out.width = img.width;
        out.height = img.height;
    } else {
        // Scale down to the size providers recommend and re-encode as PNG; a second, smaller try when it is still too big.
        bool done = false;
        for (int side : { kMaxImageSide, 1024, 640 }) {
            Image work = ImageCopy(img);
            const int m = std::max(work.width, work.height);
            if (m > side) {
                const float k = (float)side / (float)m;
                ImageResize(&work, std::max(1, (int)(work.width * k)), std::max(1, (int)(work.height * k)));
            }
            int size = 0;
            unsigned char* png = ExportImageToMemory(work, ".png", &size);
            if (png && size > 0 && size <= 4 * 1024 * 1024) {
                out.data = Base64Encode(std::string(reinterpret_cast<const char*>(png), (size_t)size));
                out.mime = "image/png";
                out.width = work.width;
                out.height = work.height;
                done = true;
            }
            if (png) MemFree(png);
            UnloadImage(work);
            if (done) break;
        }
        if (!done) { UnloadImage(img); error = "That image is too large to send."; return false; }
    }

    // A small preview for the chip in the chat box.
    Image thumb = ImageCopy(img);
    const float k = 48.0f / (float)std::max(thumb.width, thumb.height);
    if (k < 1.0f) ImageResize(&thumb, std::max(1, (int)(thumb.width * k)), std::max(1, (int)(thumb.height * k)));
    ImageFormat(&thumb, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    out.thumbW = thumb.width;
    out.thumbH = thumb.height;
    out.thumb.assign(static_cast<unsigned char*>(thumb.data), static_cast<unsigned char*>(thumb.data) + (size_t)thumb.width * thumb.height * 4);
    UnloadImage(thumb);
    UnloadImage(img);
    out.image = true;
    return true;
}
}

bool LoadAttachment(const std::string& path, bool asImage, Attachment& out, std::string& error) {
    out = Attachment{};
    const fs::path file = U8Path(path);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) { error = "That file could not be found."; return false; }
    out.name = file.filename().string();
    if (asImage) return LoadImageAttachment(file, path, out, error);

    const uintmax_t size = fs::file_size(file, ec);
    if (ec) { error = "That file could not be read."; return false; }
    if (size > kMaxTextAttachment) {
        error = out.name + " is " + std::to_string(size / 1024) + " KB; the limit is " + std::to_string(kMaxTextAttachment / 1024) + " KB.";
        return false;
    }
    std::string bytes;
    if (!ReadFileBytes(file, bytes)) { error = "That file could not be read."; return false; }
    const size_t probe = std::min<size_t>(bytes.size(), 8192);
    if (std::find(bytes.begin(), bytes.begin() + (long)probe, '\0') != bytes.begin() + (long)probe) {
        error = out.name + " is not a text file. Use Attach image for pictures.";
        return false;
    }
    if (bytes.compare(0, 3, "\xEF\xBB\xBF") == 0) bytes.erase(0, 3);   // a UTF-8 byte-order mark
    out.data = std::move(bytes);
    return true;
}

} // namespace ai
