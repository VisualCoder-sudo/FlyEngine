#pragma once
#include <string>
#include <thread>
#include <functional>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sstream>
#include <regex>
#include <cstdio>
#include <cctype>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "miniz.h"

namespace FlyEngine {

class FlyCloudModule {
public:
    using LogCallback = std::function<void(const std::string& message, bool is_error)>;
    inline static const std::string DEFAULT_TAILSCALE_URL = "http://100.66.223.107:8000";

    static void DispatchCommandAsync(
        const std::string& full_command,
        const std::string& project_root,
        LogCallback on_log,
        const std::string& project_name_hint = ""
    ) {
        std::thread([=]() {
            try {
                std::vector<std::string> args = Tokenize(full_command);
                if (args.empty() || args[0] != "fcloud") return;

                std::string config_path = project_root + "/fcloud_config.json";
                nlohmann::json cfg = LoadConfig(config_path);
                std::string server_url = ConfigGetString(cfg, "server_url", DEFAULT_TAILSCALE_URL);
                std::string api_key = ConfigGetString(cfg, "api_key", "");
                // A project name chosen with 'fcloud change --project --name' is
                // stored in the config and wins over the local folder name, so
                // pushes/deletes keep targeting the renamed cloud project.
                std::string project_name = FirstNonEmpty(
                    ConfigGetString(cfg, "project_name_override", ""),
                    project_name_hint,
                    ConfigGetString(cfg, "project_name", "FlyEngineProject"));

            // 1. fcloud --help
            if (HasArg(args, "--help")) {
                PrintHelp(on_log);
                return;
            }

            // 2. fcloud --apikey
            if (args.size() == 2 && args[1] == "--apikey") {
                if (api_key.empty()) {
                    on_log("! No API key configured in fcloud_config.json", true);
                } else {
                    std::string masked = api_key.length() > 8 ? api_key.substr(0, 4) + "..." + api_key.substr(api_key.length() - 4) : "******";
                    on_log("🔑 Active API Key: " + masked, false);
                }
                return;
            }

            // 3. fcloud change --apikey "<NEW_KEY>"
            if (args.size() >= 3 && args[1] == "change" && args[2] == "--apikey") {
                std::string new_key = args.size() >= 4 ? args[3] : "";
                ChangeKey(config_path, cfg, new_key, on_log);
                return;
            }

            // 3.5 fcloud change --project --name "<NEW_NAME>" - rename the cloud
            //     project and point this local project's config at the new name.
            if (args.size() >= 5 && args[1] == "change" && args[2] == "--project" && args[3] == "--name") {
                std::string new_name = args[4];
                RenameProject(config_path, cfg, server_url, api_key, project_name, new_name, on_log);
                return;
            }

            // 4. fcloud --apikey_limits
            if (HasArg(args, "--apikey_limits")) {
                QueryStorageLimits(server_url, api_key, on_log);
                return;
            }

            // 4.5 fcloud --check_server
            if (HasArg(args, "--check_server")) {
                CheckServerOnline(server_url, on_log);
                return;
            }

            // 5. fcloud revoke --apikey
            if (args.size() >= 3 && args[1] == "revoke" && args[2] == "--apikey") {
                RevokeKey(config_path, cfg, on_log);
                return;
            }

            // 6. fcloud push [-v <ver>] [-m <msg>]
            if (args.size() >= 2 && args[1] == "push") {
                std::string msg = GetArgValue(args, "-m", "Auto-update build");
                std::string version = GetArgValue(args, "-v", "");

                if (!PreflightCheck(server_url, api_key, on_log)) return;

                if (version.empty()) {
                    // The server's commit history is the source of truth for
                    // what is actually deployed. If nobody has ever pushed this
                    // project (or the server was reset), start over at 1.0.0
                    // instead of blindly continuing from the stale local count.
                    std::string server_latest;
                    bool server_has_versions = false;
                    bool server_ok = QueryServerLatestVersion(server_url, api_key, project_name, server_latest, server_has_versions);

                    if (server_ok && !server_has_versions) {
                        version = "1.0.0";
                        on_log("ℹ No versions on the server for this project - starting at v" + version + ".", false);
                    } else if (server_ok) {
                        version = IncrementPatchVersion(server_latest);
                        on_log("ℹ Auto-incremented release version to v" + version + " (newest on server: v" + server_latest + ").", false);
                    } else {
                        std::string current_ver = ConfigGetString(cfg, "current_version", "1.0.0");
                        version = IncrementPatchVersion(current_ver);
                        on_log("ℹ Could not read newest version from the server; incremented local version to v" + version + ".", false);
                    }

                    cfg["current_version"] = version;
                    SaveConfig(config_path, cfg);
                } else {
                    cfg["current_version"] = version;
                    SaveConfig(config_path, cfg);
                }

                ExecutePush(project_root, server_url, api_key, project_name, version, msg, on_log);
                return;
            }

            // 7. fcloud delete <...> - remove versions from the server's
            //    version control. Multiple deletion forms share one endpoint.
            if (args.size() >= 2 && args[1] == "delete") {
                DispatchDelete(args, server_url, api_key, project_name, on_log);
                return;
            }

            on_log("✕ Unknown fcloud command. Type 'fcloud --help' for available options.", true);
            } catch (const std::exception& e) {
                on_log("✕ Error handling fcloud command: " + std::string(e.what()), true);
            } catch (...) {
                on_log("✕ Unexpected error handling fcloud command.", true);
            }
        }).detach();
    }

private:
    static void PrintHelp(LogCallback on_log) {
        on_log("FlyCloud Command Syntax:", false);
        on_log("  fcloud --help                           Print command usage", false);
        on_log("  fcloud --apikey                         Display active API Key info", false);
        on_log("  fcloud change --apikey \"<key>\"          Update API key in local config", false);
        on_log("  fcloud change --project --name \"<name>\"  Rename project in cloud + local config", false);
        on_log("  fcloud --apikey_limits                  Check account storage quota", false);
        on_log("  fcloud --check_server                   Check if the server is online (5s timeout)", false);
        on_log("  fcloud revoke --apikey                  Unlink local API key", false);
        on_log("  fcloud push -m \"<msg>\"                  Deploy build (auto-increments version)", false);
        on_log("  fcloud push -v 1.5.3 -m \"<msg>\"         Deploy build with explicit version", false);
        on_log("  fcloud delete --previous                Delete the latest version", false);
        on_log("  fcloud delete --previous <n>            Delete the latest n versions", false);
        on_log("  fcloud delete --previous --oldest <n>   Delete the first n (oldest) versions", false);
        on_log("  fcloud delete --version <v>             Delete a specific version", false);
        on_log("  fcloud delete --previous --all -y       Delete ALL versions (no prompt)", false);
        on_log("  fcloud delete --project -y              Delete the WHOLE cloud project (server only)", false);
    }

    static void ChangeKey(const std::string& config_path, nlohmann::json& cfg, const std::string& new_key, LogCallback on_log) {
        if (new_key.empty()) {
            on_log("✕ Error: No API key provided. Usage: fcloud change --apikey \"YOUR_NEW_KEY\"", true);
            return;
        }
        cfg["api_key"] = new_key;
        SaveConfig(config_path, cfg);
        std::string masked = new_key.length() > 8 ? new_key.substr(0, 4) + "..." + new_key.substr(new_key.length() - 4) : "******";
        on_log("🔑 API key successfully updated to: " + masked, false);
    }

    // Renames the cloud project and records the new name locally so every
    // future fcloud command keeps targeting it. The server moves the version
    // history (zip files + commit rows) so nothing in the game breaks.
    static void RenameProject(
        const std::string& config_path,
        nlohmann::json& cfg,
        const std::string& server_url,
        const std::string& api_key,
        const std::string& project_name,
        const std::string& new_name,
        LogCallback on_log
    ) {
        if (api_key.empty()) {
            on_log("✕ No API key configured. Set one with 'fcloud change --apikey \"KEY\"' and retry.", true);
            return;
        }

        std::string trimmed = TrimRenderName(new_name);
        if (trimmed.empty()) {
            on_log("✕ Error: No project name provided. Usage: fcloud change --project --name \"NEW_NAME\"", true);
            return;
        }
        if (!std::regex_match(trimmed, std::regex(R"(^[A-Za-z0-9 _-]{1,64}$)"))) {
            on_log("✕ Error: Project name may only contain letters, numbers, spaces, hyphens, and underscores (max 64 chars).", true);
            return;
        }
        if (trimmed == project_name) {
            on_log("✕ Project is already named '" + project_name + "'.", true);
            return;
        }

        nlohmann::json body;
        body["project_name"] = project_name;
        body["new_name"] = trimmed;
        std::string payload = body.dump();

        on_log("⚡ Renaming project '" + project_name + "' to '" + trimmed + "' on " + server_url + " ...", false);

        CURL* curl = curl_easy_init();
        long http_code = 0;
        std::string response_buf;
        char errbuf[CURL_ERROR_SIZE] = {0};
        if (!curl) {
            on_log("✕ Could not initialize curl.", true);
            return;
        }

        struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/api/rename-project").c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
            data->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

        CURLcode res = curl_easy_perform(curl);
        bool ok = (res == CURLE_OK);
        if (ok) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (!ok) {
            on_log("✕ Server unreachable at " + server_url + " (" + curl_easy_strerror(res) + ").", true);
            return;
        }

        if (http_code == 200) {
            cfg["project_name"] = trimmed;
            cfg["project_name_override"] = trimmed;
            SaveConfig(config_path, cfg);
            on_log("✓ Project renamed to '" + trimmed + "'. Local config updated - future pushes and deletes will use the new name.", false);
            return;
        }

        if (http_code == 301 || http_code == 401 || http_code == 403) {
            std::string detail = ExtractDetail(response_buf);
            std::string reason = detail.empty() ? "API key rejected by the server" : detail;
            on_log("✕ " + reason + ". Check the key with 'fcloud --apikey'.", true);
            return;
        }

        if (http_code == 404) {
            std::string detail = ExtractDetail(response_buf);
            on_log("✕ " + (detail.empty() ? "Project not found on the server." : detail), true);
            std::vector<std::string> names = FetchProjectNames(server_url, api_key);
            if (!names.empty()) {
                std::string list = "Available projects on this account: ";
                for (size_t i = 0; i < names.size(); ++i) {
                    if (i) list += ", ";
                    list += "'" + names[i] + "'";
                }
                on_log(list, false);
            } else {
                on_log("No projects found on this account. Push a version first with 'fcloud push'.", false);
            }
            return;
        }

        std::string suffix = ExtractDetail(response_buf);
        std::string extra = suffix.empty() ? "" : " Server says: " + suffix;
        on_log("✕ Rename failed (HTTP " + std::to_string(http_code) + ")." + extra, true);
    }

    // Returns the list of cloud project names owned by the current API key
    // (used to guide the user when a rename target can't be found).
    static std::vector<std::string> FetchProjectNames(const std::string& server_url, const std::string& api_key) {
        std::vector<std::string> names;
        CURL* curl = curl_easy_init();
        if (!curl) return names;
        std::string response_buf;
        long http_code = 0;
        struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/api/projects").c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
            data->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
        if (curl_easy_perform(curl) == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (http_code == 200) {
            try {
                auto j = nlohmann::json::parse(response_buf);
                if (j.contains("projects") && j["projects"].is_array()) {
                    for (const auto& p : j["projects"]) {
                        if (p.contains("name") && p["name"].is_string()) names.push_back(p["name"].get<std::string>());
                    }
                }
            } catch (...) {}
        }
        return names;
    }

    // Strips leading/trailing whitespace (matching the server's rule that
    // project names may not be padded).
    static std::string TrimRenderName(const std::string& s) {
        size_t first = 0;
        while (first < s.size() && (s[first] == ' ' || s[first] == '\t')) ++first;
        size_t last = s.size();
        while (last > first && (s[last - 1] == ' ' || s[last - 1] == '\t')) --last;
        return s.substr(first, last - first);
    }

    static void QueryStorageLimits(const std::string& server_url, const std::string& api_key, LogCallback on_log) {
        on_log("⚡ Querying quota from " + server_url + "...", false);
        CURL* curl = curl_easy_init();
        std::string response_buf;
        long http_code = 0;

        if (curl) {
            struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
            curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/api/preflight").c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
                data->append((char*)ptr, sz * nm);
                return sz * nm;
            });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

            if (curl_easy_perform(curl) == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            }
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        if (http_code == 200) {
            try {
                auto data = nlohmann::json::parse(response_buf);
                float used = data.value("used_mb", 0.0f);
                float cap = data.value("cap_mb", 100.0f);
                on_log("📊 Storage: " + FormatMegabytes(cap - used) + " Remaining (" + FormatMegabytes(used) + " used of " + FormatMegabytes(cap) + ")", false);
            } catch (...) {
                on_log("✓ Account verified. Storage limit within threshold.", false);
            }
        } else {
            std::string suffix = ExtractDetail(response_buf);
            std::string extra = suffix.empty() ? "" : " Server says: " + suffix;
            on_log("✕ Quota check failed (HTTP " + std::to_string(http_code) + ")." + extra, true);
        }
    }

    // Pings the server root endpoint and reports whether it responded within
    // 5 seconds.
    static void CheckServerOnline(const std::string& server_url, LogCallback on_log) {
        on_log("⚡ Checking server at " + server_url + " ...", false);
        CURL* curl = curl_easy_init();
        if (!curl) {
            on_log("✕ Could not initialize curl.", true);
            return;
        }

        long http_code = 0;
        double total_s = 0.0;
        char errbuf[CURL_ERROR_SIZE] = {0};
        curl_easy_setopt(curl, CURLOPT_URL, server_url.c_str());
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, void* userdata) -> size_t {
            (void)ptr; (void)userdata;
            return sz * nm;
        });

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total_s);
        }
        curl_easy_cleanup(curl);

        if (res == CURLE_OPERATION_TIMEDOUT) {
            on_log("✕ Server did not respond within 5 seconds at " + server_url + ".", true);
        } else if (res != CURLE_OK) {
            std::string why = errbuf[0] ? std::string(errbuf) : std::string(curl_easy_strerror(res));
            on_log("✕ Server unreachable at " + server_url + " (" + why + ").", true);
        } else {
            char ms[32];
            std::snprintf(ms, sizeof(ms), "%.0f", total_s * 1000.0);
            on_log("✓ Server online at " + server_url + " (HTTP " + std::to_string(http_code) + ", " + ms + " ms).", false);
        }
    }

    static void RevokeKey(const std::string& config_path, nlohmann::json& cfg, LogCallback on_log) {
        cfg["api_key"] = "";
        SaveConfig(config_path, cfg);
        on_log("🧹 API key successfully revoked and removed from local project config.", false);
    }

    // Verifies the server is reachable, the API key is accepted, and there is
    // storage headroom before committing to an upload.
    static bool PreflightCheck(const std::string& server_url, const std::string& api_key, LogCallback on_log) {
        if (api_key.empty()) {
            on_log("✕ No API key configured. Set one with 'fcloud change --apikey \"KEY\"' and retry.", true);
            return false;
        }
        on_log("⚡ Checking server + API key...", false);
        CURL* curl = curl_easy_init();
        std::string response_buf;
        long http_code = 0;
        if (!curl) {
            on_log("✕ Could not initialize curl.", true);
            return false;
        }

        struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
        curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/api/preflight").c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
            data->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

        CURLcode res = curl_easy_perform(curl);
        bool reachable = (res == CURLE_OK);
        if (reachable) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (!reachable) {
            on_log("✕ Server unreachable at " + server_url + " (" + curl_easy_strerror(res) + "). Aborting push.", true);
            return false;
        }

        // The server signals an invalid/unverified key with HTTP 301 and a
        // JSON detail body (no Location header) rather than 401/403.
        if (http_code == 301 || http_code == 401 || http_code == 403) {
            std::string detail = ExtractDetail(response_buf);
            std::string reason = detail.empty()
                ? ("API key rejected by the server (HTTP " + std::to_string(http_code) + ")")
                : detail;
            on_log("✕ " + reason + ". Check the key with 'fcloud --apikey'.", true);
            return false;
        }
        if (http_code != 200) {
            std::string suffix = ExtractDetail(response_buf);
            std::string extra = suffix.empty() ? "" : " Server says: " + suffix;
            on_log("✕ Preflight failed (HTTP " + std::to_string(http_code) + ")." + extra, true);
            return false;
        }

        try {
            auto data = nlohmann::json::parse(response_buf);
            float used = data.value("used_mb", 0.0f);
            float cap = data.value("cap_mb", 100.0f);
            if (used >= cap) {
                on_log("✕ Storage quota full (" + std::to_string(used) + " / " + std::to_string(cap) + " MB). Aborting push.", true);
                return false;
            }
            on_log("✓ Server online, API key accepted, " + FormatMegabytes(cap - used) + " Remaining.", false);
        } catch (...) {
            on_log("✓ Server online, API key accepted.", false);
        }
        return true;
    }

    static void ExecutePush(
        const std::string& project_root,
        const std::string& server_url,
        const std::string& api_key,
        const std::string& project_name,
        const std::string& version,
        const std::string& message,
        LogCallback on_log
    ) {
        on_log("🚀 Packaging & Deploying v" + version + " to " + server_url + "...", false);
        if (!std::filesystem::exists(project_root)) {
            on_log("✕ Project path does not exist: " + project_root, true);
            return;
        }
        std::string temp_zip = project_root + "/.temp_export.zip";

        mz_zip_archive zip{};
        mz_zip_writer_init_file(&zip, temp_zip.c_str(), 0);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(project_root)) {
            if (!entry.is_regular_file()) continue;
            std::string path = entry.path().generic_string();
            std::string rel = std::filesystem::relative(entry.path(), project_root).generic_string();
            if (rel.rfind(".", 0) == 0 || rel.find(".import") != std::string::npos) continue;
            mz_zip_writer_add_file(&zip, rel.c_str(), path.c_str(), NULL, 0, MZ_BEST_COMPRESSION);
        }
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);

        CURL* curl = curl_easy_init();
        long http_code = 0;
        CURLcode perform_code = CURLE_FAILED_INIT;
        std::string response_buf;
        char errbuf[CURL_ERROR_SIZE] = {0};
        if (curl) {
            curl_mime* mime = curl_mime_init(curl);
            curl_mimepart* part = curl_mime_addpart(mime);
            curl_mime_name(part, "file");
            curl_mime_filedata(part, temp_zip.c_str());

            struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
            std::string endpoint = server_url + "/api/upload?project_name=" + UrlEncode(project_name) + "&version=" + UrlEncode(version) + "&message=" + UrlEncode(message);

            curl_easy_setopt(curl, CURLOPT_URL, endpoint.c_str());
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
                data->append((char*)ptr, sz * nm);
                return sz * nm;
            });
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

            perform_code = curl_easy_perform(curl);
            if (perform_code == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            }
            curl_mime_free(mime);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
        }

        std::filesystem::remove(temp_zip);
        if (http_code == 200) {
            on_log("✨ SUCCESS: Build v" + version + " published to FlyCloud!", false);
        } else if (http_code == 308) {
            std::string detail = ExtractDetail(response_buf);
            std::string msg = detail.empty()
                ? "Conflicting project name - another build with the name '" + project_name + "' already exists"
                : detail;
            on_log("✕ " + msg + " (HTTP 308)", true);
        } else if (http_code == 0 && perform_code != CURLE_OK) {
            std::string why = (perform_code == CURLE_OPERATION_TIMEDOUT)
                ? "timed out (server stalled or the connection was interrupted mid-upload)"
                : std::string(curl_easy_strerror(perform_code));
            on_log("✕ Upload failed: " + why + ".", true);
        } else {
            std::string suffix = ExtractDetail(response_buf);
            std::string extra = suffix.empty() ? "" : " Server says: " + suffix;
            std::string curlerr = (http_code == 0 && errbuf[0]) ? " curl: " + std::string(errbuf) : "";
            on_log("✕ Upload failed (HTTP " + std::to_string(http_code) + ")." + extra + curlerr, true);
        }
    }

    // Parses the fcloud delete subcommands and calls the server's /api/delete
    // endpoint with the appropriate mode/count/version.
    static void DispatchDelete(
        const std::vector<std::string>& args,
        const std::string& server_url,
        const std::string& api_key,
        const std::string& project_name,
        LogCallback on_log
    ) {
        if (api_key.empty()) {
            on_log("✕ No API key configured. Set one with 'fcloud change --apikey \"KEY\"' and retry.", true);
            return;
        }

        // fcloud delete --version <VERSION>
        if (HasArg(args, "--version")) {
            std::string ver = GetArgValue(args, "--version", "");
            if (ver.empty() || !std::regex_match(ver, std::regex(R"(\d+\.\d+\.\d+)"))) {
                on_log("✕ Usage: fcloud delete --version <major.minor.patch>", true);
                return;
            }
            DeleteFromServer(server_url, api_key, project_name, "version", 1, ver, on_log);
            return;
        }

        // fcloud delete --previous --all [-y]
        if (HasArg(args, "--all")) {
            DeleteFromServer(server_url, api_key, project_name, "all", 1, "", on_log);
            return;
        }

        // fcloud delete --previous --oldest [N]
        if (HasArg(args, "--oldest")) {
            int n = 1;
            std::string raw = GetArgValue(args, "--oldest", "");
            if (!raw.empty()) {
                if (!IsPositiveInt(raw)) {
                    on_log("✕ Usage: fcloud delete --previous --oldest <number>", true);
                    return;
                }
                n = std::stoi(raw);
            }
            DeleteFromServer(server_url, api_key, project_name, "oldest", n, "", on_log);
            return;
        }

        // fcloud delete --previous [N]  (defaults to deleting the newest 1)
        if (HasArg(args, "--previous")) {
            int n = 1;
            std::string raw = GetArgValue(args, "--previous", "");
            if (!raw.empty()) {
                if (!IsPositiveInt(raw)) {
                    on_log("✕ Usage: fcloud delete --previous <number>", true);
                    return;
                }
                n = std::stoi(raw);
            }
            DeleteFromServer(server_url, api_key, project_name, "previous", n, "", on_log);
            return;
        }

        // fcloud delete --project [-y] - delete the entire cloud project
        // (all versions + the project's storage on the server). Local files
        // are never touched.
        if (HasArg(args, "--project")) {
            if (!HasArg(args, "-y")) {
                on_log("✕ Deleting the whole project is permanent. Add '-y' to confirm: fcloud delete --project -y", true);
                return;
            }
            DeleteFromServer(server_url, api_key, project_name, "project", 1, "", on_log);
            return;
        }

        on_log("✕ Unknown delete form. Try 'fcloud --help'.", true);
    }

    static bool IsPositiveInt(const std::string& s) {
        if (s.empty()) return false;
        for (char c : s) if (c < '0' || c > '9') return false;
        return true;
    }

    // Sends a JSON delete request to /api/delete and logs the outcome.
    static void DeleteFromServer(
        const std::string& server_url,
        const std::string& api_key,
        const std::string& project_name,
        const std::string& mode,
        int count,
        const std::string& version,
        LogCallback on_log
    ) {
        nlohmann::json body;
        body["project_name"] = project_name;
        body["mode"] = mode;
        body["count"] = count;
        if (!version.empty()) body["version"] = version;
        std::string payload = body.dump();

        on_log("⚡ Deleting from " + server_url + " ...", false);

        CURL* curl = curl_easy_init();
        long http_code = 0;
        std::string response_buf;
        char errbuf[CURL_ERROR_SIZE] = {0};
        if (!curl) {
            on_log("✕ Could not initialize curl.", true);
            return;
        }

        struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, (server_url + "/api/delete").c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload.size());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
            data->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

        CURLcode res = curl_easy_perform(curl);
        bool ok = (res == CURLE_OK);
        if (ok) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (!ok) {
            on_log("✕ Server unreachable at " + server_url + " (" + curl_easy_strerror(res) + ").", true);
            return;
        }

        if (http_code == 301 || http_code == 401 || http_code == 403) {
            std::string detail = ExtractDetail(response_buf);
            std::string reason = detail.empty() ? "API key rejected by the server" : detail;
            on_log("✕ " + reason + ". Check the key with 'fcloud --apikey'.", true);
            return;
        }

        if (http_code == 200) {
            try {
                auto data = nlohmann::json::parse(response_buf);
                std::string msg = data.value("message", "");
                int n = data.value("deleted_count", 0);
                if (!msg.empty()) on_log("🗑 " + msg, false);
                if (n == 0) on_log("🗑 No versions were deleted.", false);
            } catch (...) {
                on_log("🗑 Versions deleted successfully.", false);
            }
        } else {
            std::string suffix = ExtractDetail(response_buf);
            std::string extra = suffix.empty() ? "" : " Server says: " + suffix;
            on_log("✕ Delete failed (HTTP " + std::to_string(http_code) + ")." + extra, true);
        }
    }

    // Formats a size in MB as a friendly, rounded string: "86.3MB", "1.8GB".
    static std::string FormatMegabytes(float mb) {
        char buf[32];
        if (mb >= 1024.0f) {
            std::snprintf(buf, sizeof(buf), "%.1fGB", mb / 1024.0f);
        } else {
            std::snprintf(buf, sizeof(buf), "%.1fMB", mb);
        }
        return buf;
    }

    static std::string UrlEncode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += (char)c;
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                out += buf;
            }
        }
        return out;
    }

    static std::string IncrementPatchVersion(const std::string& ver) {
        int major = 1, minor = 0, patch = 0;
        std::sscanf(ver.c_str(), "%d.%d.%d", &major, &minor, &patch);
        patch++;
        if (patch > 9) {
            patch = 0;
            minor++;
            if (minor > 9) {
                minor = 0;
                major++;
            }
        }
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    // Numeric comparison of "major.minor.patch" versions (string comparison is
    // wrong once any segment reaches two digits, e.g. 1.10.0 vs 1.9.0).
    static bool VersionGreater(const std::string& a, const std::string& b) {
        int am[3] = {0, 0, 0}, bm[3] = {0, 0, 0};
        std::sscanf(a.c_str(), "%d.%d.%d", &am[0], &am[1], &am[2]);
        std::sscanf(b.c_str(), "%d.%d.%d", &bm[0], &bm[1], &bm[2]);
        for (int i = 0; i < 3; ++i) {
            if (am[i] != bm[i]) return am[i] > bm[i];
        }
        return false;
    }

    // Asks the server which version is currently the newest committed one for
    // this project. out_has_versions is false when the server has none (e.g.
    // the account/project was reset). Returns false only if the server could
    // not be reached or rejected the request.
    static bool QueryServerLatestVersion(
        const std::string& server_url,
        const std::string& api_key,
        const std::string& project_name,
        std::string& out_latest,
        bool& out_has_versions
    ) {
        out_latest.clear();
        out_has_versions = false;

        CURL* curl = curl_easy_init();
        if (!curl) return false;
        std::string response_buf;
        long http_code = 0;

        struct curl_slist* headers = curl_slist_append(NULL, ("Authorization: Bearer " + api_key).c_str());
        std::string url = server_url + "/api/commits?project_name=" + UrlEncode(project_name);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void* ptr, size_t sz, size_t nm, std::string* data) -> size_t {
            data->append((char*)ptr, sz * nm);
            return sz * nm;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);

        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK || http_code != 200) return false;

        try {
            auto j = nlohmann::json::parse(response_buf);
            if (!j.contains("commits") || !j["commits"].is_array()) return false;
            std::string latest;
            for (const auto& c : j["commits"]) {
                if (!c.is_object() || !c.contains("version") || !c["version"].is_string()) continue;
                const std::string v = c["version"].get<std::string>();
                if (latest.empty() || VersionGreater(v, latest)) latest = v;
            }
            out_latest = latest;
            out_has_versions = !latest.empty();
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::vector<std::string> Tokenize(const std::string& str) {
        std::vector<std::string> tokens;
        std::regex re(R"([^\s"]+|"[^"]*")");
        auto begin = std::sregex_iterator(str.begin(), str.end(), re);
        auto end = std::sregex_iterator();
        for (auto i = begin; i != end; ++i) {
            std::string t = i->str();
            if (t.front() == '"' && t.back() == '"') t = t.substr(1, t.length() - 2);
            tokens.push_back(t);
        }
        return tokens;
    }

    static bool HasArg(const std::vector<std::string>& args, const std::string& flag) {
        for (const auto& a : args) if (a == flag) return true;
        return false;
    }

    static std::string GetArgValue(const std::vector<std::string>& args, const std::string& flag, const std::string& def) {
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == flag && i + 1 < args.size()) return args[i + 1];
        }
        return def;
    }

    // Pulls the "detail" field out of a JSON API error body.
    static std::string ExtractDetail(const std::string& body) {
        try {
            auto j = nlohmann::json::parse(body);
            if (j.contains("detail") && j["detail"].is_string()) return j["detail"].get<std::string>();
        } catch (...) {}
        return "";
    }

    static nlohmann::json LoadConfig(const std::string& path) {
        if (!std::filesystem::exists(path)) return nlohmann::json::object();
        try { std::ifstream f(path); return nlohmann::json::parse(f); } catch (...) { return nlohmann::json::object(); }
    }

    static std::string ConfigGetString(const nlohmann::json& cfg, const std::string& key, const std::string& def) {
        auto it = cfg.find(key);
        if (it != cfg.end() && it->is_string()) return it->get<std::string>();
        return def;
    }

    // Returns the first non-empty argument, "" if all are empty.
    static std::string FirstNonEmpty(const std::string& a, const std::string& b, const std::string& c) {
        if (!a.empty()) return a;
        if (!b.empty()) return b;
        return c;
    }

    static void SaveConfig(const std::string& path, const nlohmann::json& cfg) {
        std::ofstream f(path);
        f << cfg.dump(4);
    }
};

} // namespace FlyEngine