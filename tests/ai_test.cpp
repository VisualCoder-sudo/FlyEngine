// AI assistant without a network: settings default to off and round-trip with owner-only permissions, the Claude
// request/response JSON, the Apply-block parser, and the checks that decide what Apply may do. No window.
#include "test_unique.hpp"
#include "Engine/AI/AI.hpp"
#include "Engine/Scripts/ScriptCompiler.hpp"
#include "raylib.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    const fs::path dir = fs::temp_directory_path() / TestUnique("fly_ai_test");
    fs::remove_all(dir);
    const std::string cfg = (dir / "ai.cfg").string();
    ai::SetConfigPathForTesting(cfg);
    ai::SetChatsDirForTesting((dir / "chats_default").string());   // a Send saves its chat: never into the real config folder

    // Off by default, and loading a missing file creates nothing.
    ai::Load();
    CHECK(!ai::GetSettings().enabled);
    CHECK(ai::GetSettings().key[0].empty() && ai::GetSettings().provider == ai::ProviderId::Claude);
    CHECK(!fs::exists(cfg));
    CHECK(!ai::Active());

    // Round trip + owner-only permissions.
    unsetenv("ANTHROPIC_API_KEY"); unsetenv("OPENAI_API_KEY"); unsetenv("GEMINI_API_KEY"); unsetenv("GOOGLE_API_KEY");
    ai::GetSettings().enabled = true;
    ai::GetSettings().key[0] = "sk-ant-test-1234567890";
    ai::GetSettings().key[1] = "sk-openai-test-abcdef";
    ai::GetSettings().key[2] = "gemini-test-key";
    ai::GetSettings().model[2] = "gemini-custom-1";
    ai::GetSettings().provider = ai::ProviderId::Gemini;
    ai::GetSettings().model[0] = "claude-haiku-5-5";
    ai::Save();
    CHECK(fs::exists(cfg));
#if !defined(_WIN32)
    CHECK((fs::status(cfg).permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    ai::GetSettings() = ai::Settings{};
    ai::Load();
    CHECK(ai::GetSettings().enabled);
    CHECK(ai::GetSettings().key[0] == "sk-ant-test-1234567890" && ai::GetSettings().key[1] == "sk-openai-test-abcdef");
    CHECK(ai::GetSettings().provider == ai::ProviderId::Gemini && ai::GetSettings().key[2] == "gemini-test-key" && ai::GetSettings().model[2] == "gemini-custom-1");
    CHECK(ai::EffectiveKey() == "gemini-test-key" && ai::SelectedModel() == "gemini-custom-1");
    ai::GetSettings().provider = ai::ProviderId::Claude;
    CHECK(ai::GetSettings().model[0] == "claude-haiku-5-5");
    CHECK(ai::Active());

    // Env var wins, and the key is never shown in full.
    setenv("ANTHROPIC_API_KEY", "sk-ant-from-env", 1);
    CHECK(ai::KeyFromEnvironment());
    CHECK(ai::EffectiveKey() == "sk-ant-from-env");
    unsetenv("ANTHROPIC_API_KEY");
    CHECK(ai::MaskKey("sk-ant-test-1234567890").find("test-12345") == std::string::npos);

    // Disabled again: Active() is false even with a key.
    ai::GetSettings().enabled = false;
    CHECK(!ai::Active());

    // Request JSON.
    ai::ChatRequest req;
    req.systemStatic = "DOCS"; req.systemDynamic = "CTX"; req.model = "claude-sonnet-5-5"; req.maxTokens = 99;
    req.messages = { { true, "hi" }, { false, "hello" }, { true, "make a cube" } };
    const auto j = nlohmann::json::parse(ai::BuildClaudeRequestJson(req));
    CHECK(j["model"] == "claude-sonnet-5-5" && j["max_tokens"] == 99);
    CHECK(j["system"].size() == 2 && j["system"][0]["cache_control"]["type"] == "ephemeral" && !j["system"][1].contains("cache_control"));
    CHECK(j["messages"].size() == 3 && j["messages"][1]["role"] == "assistant" && j["messages"][2]["content"] == "make a cube");
    CHECK(!j.dump().empty() && j.dump().find("sk-ant") == std::string::npos);

    // Response JSON.
    auto ok = ai::ParseClaudeResponse(200, R"({"content":[{"type":"text","text":"A"},{"type":"text","text":"B"}]})");
    CHECK(ok.ok && ok.text == "AB");
    CHECK(!ai::ParseClaudeResponse(401, R"({"error":{"message":"bad key"}})").ok);
    CHECK(ai::ParseClaudeResponse(401, "{}").error.find("Invalid API key") != std::string::npos);
    CHECK(ai::ParseClaudeResponse(429, "{}").error.find("Rate limited") != std::string::npos);
    CHECK(!ai::ParseClaudeResponse(200, "not json").ok);
    CHECK(!ai::ParseClaudeResponse(200, R"({"content":[]})").ok);

    // An empty answer says why: all output spent thinking (retryable), a refusal, or something else (with the details).
    {
        const auto spent = ai::ParseClaudeResponse(200, R"({"content":[{"type":"thinking","thinking":"..."}],"stop_reason":"max_tokens"})");
        CHECK(!spent.ok && spent.budget && spent.error.find("output limit") != std::string::npos);
        const auto refused = ai::ParseClaudeResponse(200, R"({"content":[],"stop_reason":"refusal"})");
        CHECK(!refused.ok && !refused.budget && refused.error.find("declined") != std::string::npos);
        const auto odd = ai::ParseClaudeResponse(200, R"({"content":[{"type":"text","text":""}],"stop_reason":"end_turn"})");
        CHECK(!odd.ok && !odd.budget && odd.error.find("end_turn") != std::string::npos && odd.error.find("text") != std::string::npos);
        CHECK(ai::ParseClaudeResponse(200, R"({"content":[{"type":"thinking","thinking":"x"},{"type":"text","text":"Answer"}],"stop_reason":"end_turn"})").text == "Answer");
        CHECK(ai::ParseOpenAIResponse(200, R"({"choices":[{"message":{"content":""},"finish_reason":"length"}]})").budget);
        CHECK(ai::ParseGeminiResponse(200, R"({"candidates":[{"content":{"parts":[]},"finishReason":"MAX_TOKENS"}]})").budget);
        ai::ChatRequest big; CHECK(big.maxTokens >= 16000);
    }

    // OpenAI (Chat Completions): system message first, max_completion_tokens, choices[0].message.content.
    {
        const auto oj = nlohmann::json::parse(ai::BuildOpenAIRequestJson(req));
        CHECK(oj["model"] == "claude-sonnet-5-5" && oj["max_completion_tokens"] == 99 && !oj.contains("max_tokens"));
        CHECK(oj["messages"].size() == 4 && oj["messages"][0]["role"] == "system");
        const std::string sys = oj["messages"][0]["content"];
        CHECK(sys.find("DOCS") == 0 && sys.find("CTX") != std::string::npos);        // static docs first, for prefix caching
        CHECK(oj["messages"][2]["role"] == "assistant" && oj["messages"][3]["content"] == "make a cube");
        auto good = ai::ParseOpenAIResponse(200, R"({"choices":[{"message":{"content":"Hello"},"finish_reason":"stop"}]})");
        CHECK(good.ok && good.text == "Hello");
        auto parts = ai::ParseOpenAIResponse(200, R"({"choices":[{"message":{"content":[{"type":"text","text":"A"},{"type":"text","text":"B"}]}}]})");
        CHECK(parts.ok && parts.text == "AB");
        CHECK(ai::ParseOpenAIResponse(200, R"({"choices":[{"message":{"content":null,"refusal":"no"}}]})").error.find("refused") != std::string::npos);
        CHECK(ai::ParseOpenAIResponse(200, R"({"choices":[{"message":{"content":""},"finish_reason":"length"}]})").error.find("output limit") != std::string::npos);
        CHECK(ai::ParseOpenAIResponse(401, R"({"error":{"message":"Incorrect API key"}})").error.find("Invalid API key") != std::string::npos);
        CHECK(ai::ParseOpenAIResponse(404, R"({"error":{"message":"The model `x` does not exist"}})").error.find("Model not found") != std::string::npos);
        CHECK(ai::ParseOpenAIResponse(429, "{}").error.find("quota") != std::string::npos);
        CHECK(!ai::ParseOpenAIResponse(200, "<html>").ok && !ai::ParseOpenAIResponse(200, R"({"choices":[]})").ok);
    }

    // Gemini (generateContent): systemInstruction, contents with "model" turns, maxOutputTokens, candidates[0].content.parts.
    {
        const auto gj = nlohmann::json::parse(ai::BuildGeminiRequestJson(req));
        CHECK(gj["systemInstruction"]["parts"][0]["text"].get<std::string>().find("DOCS") == 0);
        CHECK(gj["contents"].size() == 3 && gj["contents"][1]["role"] == "model" && gj["contents"][2]["parts"][0]["text"] == "make a cube");
        CHECK(gj["generationConfig"]["maxOutputTokens"] == 99 && !gj.contains("model"));
        auto good = ai::ParseGeminiResponse(200, R"({"candidates":[{"content":{"parts":[{"text":"Hi "},{"text":"there"}]},"finishReason":"STOP"}]})");
        CHECK(good.ok && good.text == "Hi there");
        auto thought = ai::ParseGeminiResponse(200, R"({"candidates":[{"content":{"parts":[{"text":"hmm","thought":true},{"text":"Answer"}]}}]})");
        CHECK(thought.ok && thought.text == "Answer");                               // thinking summaries are not the reply
        CHECK(ai::ParseGeminiResponse(200, R"({"promptFeedback":{"blockReason":"SAFETY"}})").error.find("blocked") != std::string::npos);
        CHECK(ai::ParseGeminiResponse(200, R"({"candidates":[{"content":{"parts":[]},"finishReason":"MAX_TOKENS"}]})").error.find("output limit") != std::string::npos);
        CHECK(ai::ParseGeminiResponse(400, R"({"error":{"message":"API key not valid. Please pass a valid API key."}})").error.find("Invalid API key") != std::string::npos);
        CHECK(ai::ParseGeminiResponse(404, "{}").error.find("Model not found") != std::string::npos);
        CHECK(!ai::ParseGeminiResponse(200, "nope").ok);
    }

    // Model names end up in a URL (Gemini), so only plain characters are accepted.
    CHECK(ai::IsSafeModelId("gemini-3.8-flash") && ai::IsSafeModelId("gpt-6.1-sol") && ai::IsSafeModelId("ft:gpt-6:org::id"));
    CHECK(!ai::IsSafeModelId("") && !ai::IsSafeModelId("a/b") && !ai::IsSafeModelId("a b") && !ai::IsSafeModelId("m?key=1") && !ai::IsSafeModelId("a#b"));

    // Per-provider env vars and names.
    {
        setenv("OPENAI_API_KEY", "sk-env-openai", 1);
        setenv("GOOGLE_API_KEY", "g-env", 1);
        CHECK(ai::EnvKey(ai::ProviderId::OpenAI) == "sk-env-openai" && ai::EnvKey(ai::ProviderId::Gemini) == "g-env" && ai::EnvKey(ai::ProviderId::Claude).empty());
        setenv("GEMINI_API_KEY", "gem-env", 1);
        CHECK(ai::EnvKey(ai::ProviderId::Gemini) == "gem-env");                    // GEMINI_API_KEY wins over GOOGLE_API_KEY
        unsetenv("OPENAI_API_KEY"); unsetenv("GOOGLE_API_KEY"); unsetenv("GEMINI_API_KEY");
        for (int i = 0; i < ai::kProviderCount; ++i) {
            const auto id = static_cast<ai::ProviderId>(i);
            CHECK(!ai::DefaultModel(id).empty() && ai::IsSafeModelId(ai::DefaultModel(id)) && ai::ProviderIconIndex(id) >= 0 && ai::ProviderIconIndex(id) <= 2);
            CHECK(ai::MakeProvider(id) != nullptr);
        }
    }

    // The model lists come from the providers: one page of each, parsed into id / label / chat, with the paging cursor.
    {
        const auto cm = ai::ParseClaudeModels(200, R"({"data":[{"type":"model","id":"claude-opus-9","display_name":"Claude Opus 9","created_at":"2026-01-01T00:00:00Z"},
            {"id":"claude-haiku-9","display_name":"Claude Haiku 9"},{"id":"bad id/../x","display_name":"x"}],"has_more":true,"last_id":"claude-haiku-9"})");
        CHECK(cm.ok && cm.models.size() == 2 && cm.models[0].id == "claude-opus-9" && cm.models[0].label == "Claude Opus 9" && cm.next == "claude-haiku-9");
        CHECK(ai::ParseClaudeModels(200, R"({"data":[{"id":"a"}],"has_more":false})").next.empty());
        CHECK(ai::ParseClaudeModels(401, R"({"error":{"message":"x"}})").error.find("Invalid API key") != std::string::npos);
        CHECK(!ai::ParseClaudeModels(200, "<html>").ok);

        const auto om = ai::ParseOpenAIModels(200, R"({"data":[{"id":"text-embedding-9","created":5},{"id":"gpt-9","created":30},{"id":"whisper-9","created":1},
            {"id":"gpt-9-mini","created":20},{"id":"o9","created":10},{"id":"gpt-image-9","created":7}]})");
        CHECK(om.ok && om.models.size() == 6 && om.models[0].id == "gpt-9" && om.models[1].id == "gpt-9-mini");   // newest first
        int chatCount = 0;
        for (const auto& m : om.models) chatCount += m.chat;
        CHECK(chatCount == 3);                                                          // gpt-9, gpt-9-mini, o9; not embedding / whisper / image
        CHECK(ai::LooksLikeChatModel("gpt-6-astra") && ai::LooksLikeChatModel("o4-mini") && !ai::LooksLikeChatModel("gpt-4o-realtime-preview") && !ai::LooksLikeChatModel("tts-1"));
        CHECK(ai::ParseOpenAIModels(429, "{}").error.find("quota") != std::string::npos);

        const auto gm = ai::ParseGeminiModels(200, R"({"models":[
            {"name":"models/gemini-9-pro","displayName":"Gemini 9 Pro","supportedGenerationMethods":["generateContent","countTokens"]},
            {"name":"models/text-embedding-9","displayName":"Embedding 9","supportedGenerationMethods":["embedContent"]},
            {"name":"models/gemini-9-flash","supportedGenerationMethods":["generateContent"]}],"nextPageToken":"tok"})");
        CHECK(gm.ok && gm.models.size() == 3 && gm.models[0].id == "gemini-9-pro" && gm.models[0].label == "Gemini 9 Pro" && gm.next == "tok");
        CHECK(gm.models[0].chat && !gm.models[1].chat && gm.models[2].chat && gm.models[2].label == "gemini-9-flash");
        CHECK(ai::ParseGeminiModels(400, R"({"error":{"message":"API key not valid"}})").error.find("Invalid API key") != std::string::npos);

        // The cache: filtered choices, labels, a disk round trip, and a hand-edited cache cannot smuggle in odd names.
        ai::GetSettings().showAllModels = false;
        ai::SetModels(ai::ProviderId::OpenAI, om.models);
        CHECK(ai::ModelChoices(ai::ProviderId::OpenAI).size() == 3 && ai::ModelsFetchedAt(ai::ProviderId::OpenAI) > 0 && !ai::ModelsStale(ai::ProviderId::OpenAI));
        ai::GetSettings().showAllModels = true;
        CHECK(ai::ModelChoices(ai::ProviderId::OpenAI).size() == 6);
        ai::GetSettings().showAllModels = false;
        ai::SetModels(ai::ProviderId::Claude, cm.models);
        CHECK(ai::ModelLabel(ai::ProviderId::Claude, "claude-opus-9") == "Claude Opus 9" && ai::ModelLabel(ai::ProviderId::Claude, "unknown-x") == "unknown-x");
        CHECK(ai::ModelsStale(ai::ProviderId::Gemini));                                  // never fetched
        ai::Save(); ai::SaveModelCache();
        ai::SetModels(ai::ProviderId::OpenAI, {}); ai::SetModels(ai::ProviderId::Claude, {});
        CHECK(ai::Models(ai::ProviderId::OpenAI).empty());
        ai::LoadModelCache();
        CHECK(ai::Models(ai::ProviderId::OpenAI).size() == 6 && ai::Models(ai::ProviderId::Claude).size() == 2);
        CHECK(ai::Models(ai::ProviderId::OpenAI)[0].id == "gpt-9" && ai::Models(ai::ProviderId::OpenAI)[0].chat && !ai::Models(ai::ProviderId::OpenAI)[5].chat);
        {
            std::ofstream((dir / "ai_models.json").string()) << R"({"gemini":{"fetched":1,"models":[{"id":"ok-model"},{"id":"../../evil"},{"id":5}]}})";
            ai::LoadModelCache();
            CHECK(ai::Models(ai::ProviderId::Gemini).size() == 1 && ai::Models(ai::ProviderId::Gemini)[0].id == "ok-model");
            CHECK(ai::Models(ai::ProviderId::OpenAI).empty());                           // the file replaced the cache
            std::ofstream((dir / "ai_models.json").string()) << "{ not json";
            ai::LoadModelCache();                                                        // a damaged cache is just an empty one
            CHECK(ai::Models(ai::ProviderId::Gemini).empty());
        }
    }

    // Reply parsing.
    const std::string reply =
        "Sure.\n```fly-console\nCube.Position = (0, 5, 0)\nCube.Scale = (2,2,2)\n```\nAnd a script:\n"
        "```fly-script:Spin.cpp\n#include <fly.hpp>\n```\n```cpp\nint x;\n```\nDone.";
    const auto segs = ai::ParseReply(reply);
    CHECK(segs.size() == 6);
    if (segs.size() == 6) {
        CHECK(segs[0].kind == ai::Segment::Text);
        CHECK(segs[1].kind == ai::Segment::Console && segs[1].text == "Cube.Position = (0, 5, 0)\nCube.Scale = (2,2,2)");
        CHECK(segs[3].kind == ai::Segment::Script && segs[3].name == "Spin.cpp" && segs[3].text == "#include <fly.hpp>");
        CHECK(segs[4].kind == ai::Segment::Code);
        CHECK(segs[5].text == "Done.");
    }
    CHECK(ai::ParseReply("```fly-console\nunterminated").size() == 1);   // never becomes an Apply block

    // Lenient block headers: the name can come with a folder, no extension, a space, or from the FLY_SCRIPT class.
    {
        auto one = [](const std::string& reply) { auto v = ai::ParseReply(reply); return v.empty() ? ai::Segment{} : v[0]; };
        CHECK(one("```fly-script Spinner\ncode\n```").kind == ai::Segment::Script && one("```fly-script Spinner\ncode\n```").name == "Spinner.cpp");
        CHECK(one("```fly-script:Scripts/Spinner.cpp\ncode\n```").name == "Spinner.cpp");
        CHECK(one("```fly-script: `Spinner.cpp`\ncode\n```").name == "Spinner.cpp");
        const auto evil = one("```fly-script:../../evil.cpp\ncode\n```");
        CHECK(evil.name == "evil.cpp" && ai::IsSafeScriptName(evil.name));          // only the base name survives
        CHECK(one("```fly-script\nFLY_SCRIPT(Door)\n```").name == "Door.cpp");
        const auto plain = one("```cpp\n#include \"fly.hpp\"\nstruct A : fly::Script {};\nFLY_SCRIPT(A)\n```");
        CHECK(plain.kind == ai::Segment::Script && plain.name == "A.cpp");           // a plain cpp block that is a script gets Apply
        CHECK(one("```cpp\nint x;\n```").kind == ai::Segment::Code);
    }

    // The example script the model is shown really compiles (skipped where there is no C++ compiler), and a script that
    // uses an API that does not exist fails with a located error, which is what Apply reports back.
    if (scriptCompiler::CompilerDescription().empty()) {
        std::printf("SKIP: no C++ compiler for the script build check\n");
    } else {
        const fs::path proj = dir / "proj";
        fs::create_directories(proj / "Scripts");
        std::ofstream(proj / "Scripts" / "Spinner.cpp") << ai::ScriptTemplate();
        const auto good = scriptCompiler::Build(proj.string());
        if (!good.ok) std::printf("%s\n", good.log.c_str());
        CHECK(good.ok);
        std::ofstream(proj / "Scripts" / "Spinner.cpp") << "#include \"fly.hpp\"\nstruct Bad : fly::Script { fly::Task Run() override { fly::Object::FindByMagic(\"x\"); co_return; } };\nFLY_SCRIPT(Bad)\n";
        const auto bad = scriptCompiler::Build(proj.string());
        CHECK(!bad.ok);
        bool located = false;
        for (const auto& d : bad.diagnostics) located |= d.severity == scriptCompiler::Diagnostic::Severity::Error && d.line > 0 && d.message.find("FindByMagic") != std::string::npos;
        CHECK(located);
    }

    // AI scripts are unsaved changes: committed by a project save, undone by Discard / closing the editor, and a file you
    // edited since is never touched.
    {
        const fs::path pj = dir / "pending_proj";
        fs::create_directories(pj / "Scripts");
        const std::string P = pj.string();
        auto slurp = [](const fs::path& f) { std::ifstream in(f, std::ios::binary); std::stringstream ss; ss << in.rdbuf(); return ss.str(); };
        std::string err;

        // A new file: present while pending, gone after Discard.
        CHECK(ai::WritePendingScript(P, "Fresh.cpp", "// ai v1", err));
        CHECK(fs::exists(pj / "Scripts" / "Fresh.cpp") && ai::IsScriptPending(P, "Fresh.cpp") && ai::PendingScripts(P).size() == 1);
        CHECK(ai::WritePendingScript(P, "Fresh.cpp", "// ai v2", err));                      // applying again keeps one entry
        CHECK(ai::PendingScripts(P).size() == 1 && slurp(pj / "Scripts" / "Fresh.cpp") == "// ai v2\n");
        CHECK(ai::RevertPendingScript(P, "Fresh.cpp") == ai::PendingOutcome::Reverted);
        CHECK(!fs::exists(pj / "Scripts" / "Fresh.cpp") && !ai::IsScriptPending(P, "Fresh.cpp") && !fs::exists(pj / ".ai_pending"));
        CHECK(ai::RevertPendingScript(P, "Fresh.cpp") == ai::PendingOutcome::NotPending);

        // An existing saved file the AI overwrites (twice): Discard restores the original text.
        std::ofstream(pj / "Scripts" / "Mine.cpp") << "// my saved script\n";
        CHECK(ai::WritePendingScript(P, "Mine.cpp", "// ai takes over", err) && ai::WritePendingScript(P, "Mine.cpp", "// ai again", err));
        CHECK(slurp(pj / "Scripts" / "Mine.cpp") == "// ai again\n");
        CHECK(ai::RevertPendingScript(P, "Mine.cpp") == ai::PendingOutcome::Reverted && slurp(pj / "Scripts" / "Mine.cpp") == "// my saved script\n");

        // Edited by hand after the AI wrote it: it is yours now, Discard leaves it alone.
        CHECK(ai::WritePendingScript(P, "Edited.cpp", "// ai", err));
        std::ofstream(pj / "Scripts" / "Edited.cpp") << "// ai\n// and my own change\n";
        CHECK(ai::RevertPendingScript(P, "Edited.cpp") == ai::PendingOutcome::KeptEdited);
        CHECK(slurp(pj / "Scripts" / "Edited.cpp") == "// ai\n// and my own change\n" && !ai::IsScriptPending(P, "Edited.cpp"));

        // No record of what the AI wrote (a crash between the two writes): the file is kept, never deleted.
        CHECK(ai::WritePendingScript(P, "Lost.cpp", "// ai", err));
        fs::remove(pj / ".ai_pending" / "Lost.cpp.ai");
        CHECK(ai::RevertPendingScript(P, "Lost.cpp") == ai::PendingOutcome::KeptEdited && fs::exists(pj / "Scripts" / "Lost.cpp"));

        // Saving the project commits: the files stay and nothing is pending.
        CHECK(ai::WritePendingScript(P, "Keep.cpp", "// keep me", err) && ai::WritePendingScript(P, "Keep2.cpp", "// keep me too", err));
        ai::CommitPendingScripts(P);
        CHECK(ai::PendingScripts(P).empty() && fs::exists(pj / "Scripts" / "Keep.cpp") && fs::exists(pj / "Scripts" / "Keep2.cpp") && !fs::exists(pj / ".ai_pending"));
        CHECK(ai::RevertAllPendingScripts(P) == 0 && fs::exists(pj / "Scripts" / "Keep.cpp"));   // committed files are never reverted

        // Closing the editor / the next launch after a crash: RevertAll puts back everything still pending (state is on disk).
        std::ofstream(pj / "Scripts" / "Old.cpp") << "// old\n";
        CHECK(ai::WritePendingScript(P, "A.cpp", "// a", err) && ai::WritePendingScript(P, "B.cpp", "// b", err) && ai::WritePendingScript(P, "Old.cpp", "// ai old", err));
        CHECK(ai::RevertAllPendingScripts(P) == 3);
        CHECK(!fs::exists(pj / "Scripts" / "A.cpp") && !fs::exists(pj / "Scripts" / "B.cpp") && slurp(pj / "Scripts" / "Old.cpp") == "// old\n");

        // Bad names are refused, and a tampered journal cannot point outside Scripts/.
        CHECK(!ai::WritePendingScript(P, "../escape.cpp", "x", err) && !ai::WritePendingScript(P, "a/b.cpp", "x", err) && !ai::WritePendingScript("", "A.cpp", "x", err));
        fs::create_directories(pj / ".ai_pending");
        std::ofstream(pj / ".ai_pending" / "journal.txt") << "../victim.cpp\tnew\n";
        std::ofstream(pj / "victim.cpp") << "precious";
        CHECK(ai::PendingScripts(P).empty() && ai::RevertAllPendingScripts(P) == 0 && fs::exists(pj / "victim.cpp"));
    }

    // Questions: first line is the question, bullets / numbers are the options.
    {
        const auto q = ai::ParseReply("Which one?\n```fly-question\nWhich terrain do you want?\n- Chunked terrain\n- Brush-based BasicTerrain\n3. Something else\n```\n");
        CHECK(q.size() == 2 && q[1].kind == ai::Segment::Question && q[1].text == "Which terrain do you want?");
        if (q.size() == 2) CHECK(q[1].options.size() == 3 && q[1].options[0] == "Chunked terrain" && q[1].options[2] == "Something else");
        const auto wrapped = ai::ParseReply("```fly-question\nA long question\nthat wraps?\n* yes\n* no\n```");
        CHECK(wrapped.size() == 1 && wrapped[0].text == "A long question that wraps?" && wrapped[0].options.size() == 2);
        std::string many = "```fly-question\nQ?\n";
        for (int i = 0; i < 10; ++i) many += "- opt" + std::to_string(i) + "\n";
        CHECK(ai::ParseReply(many + "```")[0].options.size() == 6);                    // capped
        CHECK(ai::ParseReply("```fly-question\nJust asking in words\n```")[0].options.empty());
        const auto marked = ai::ParseReply("```fly-question\nQ?\n- Chunked `Terrain` (**large** worlds)\n- b\n```");
        CHECK(marked[0].options[0] == "Chunked Terrain (large worlds)");               // no markdown marks on a button label
    }

    // A reply still being typed out: an unfinished fence is a plain code block, never an Apply button.
    {
        const auto half = ai::ParseReply("Here you go:\n```fly-script:Spin.cpp\n#include \"fly.hpp\"\nstruct Spin", true);
        CHECK(half.size() == 2 && half[0].kind == ai::Segment::Text && half[1].kind == ai::Segment::Code && half[1].text.find("struct Spin") != std::string::npos);
        const auto done = ai::ParseReply("Here you go:\n```fly-script:Spin.cpp\ncode\n```\nDone.", true);
        CHECK(done.size() == 3 && done[1].kind == ai::Segment::Script);
        CHECK(ai::ParseReply("```fly-script:Spin.cpp\nunfinished").front().kind == ai::Segment::Text);   // not partial: shown as text
    }

    // The + menu becomes instructions the model must follow.
    {
        const std::string none = ai::ModeInstructions(false, false, true);
        CHECK(none.find("explanation-only") != std::string::npos && none.find("Questions are ON") != std::string::npos);
        const std::string both = ai::ModeInstructions(true, true, false);
        CHECK(both.find("Script is ON") != std::string::npos && both.find("Build is ON") != std::string::npos && both.find("Questions are OFF") != std::string::npos && both.find("explanation-only") == std::string::npos);
        const std::string scriptOnly = ai::ModeInstructions(true, false, true);
        CHECK(scriptOnly.find("Script is ON") != std::string::npos && scriptOnly.find("Build is OFF") != std::string::npos);
        ai::GetSettings() = ai::Settings{};
        CHECK(!ai::GetSettings().modeScript && !ai::GetSettings().modeBuild && ai::GetSettings().modeAsk);   // default: explain only, questions allowed
        ai::GetSettings().modeScript = true; ai::GetSettings().modeAsk = false;
        ai::SetConfigPathForTesting((dir / "modes.cfg").string());
        ai::Save(); ai::GetSettings() = ai::Settings{}; ai::Load();
        CHECK(ai::GetSettings().modeScript && !ai::GetSettings().modeBuild && !ai::GetSettings().modeAsk);
        ai::SetConfigPathForTesting((dir / "ai.cfg").string());
    }

    // Attachments: base64, files and images become part of the request; images are scaled and re-encoded only when needed.
    {
        CHECK(ai::Base64Encode("") == "" && ai::Base64Encode("f") == "Zg==" && ai::Base64Encode("fo") == "Zm8=" && ai::Base64Encode("foo") == "Zm9v" && ai::Base64Encode("foob") == "Zm9vYg==");
        CHECK(ai::Base64Encode(std::string("\xff\xfe\x00", 3)) == "//4A");
        const fs::path ad = dir / "attach";
        fs::create_directories(ad);
        std::string err;
        ai::Attachment a;

        // A text file: kept as text, BOM dropped; refused when too large, binary, or missing.
        std::ofstream(ad / "notes.cpp", std::ios::binary) << "\xEF\xBB\xBFint main() {}\n";
        CHECK(ai::LoadAttachment((ad / "notes.cpp").string(), false, a, err) && a.name == "notes.cpp" && !a.image && a.data == "int main() {}\n");
        { std::ofstream big(ad / "big.txt", std::ios::binary); big << std::string(ai::kMaxTextAttachment + 1, 'x'); }
        CHECK(!ai::LoadAttachment((ad / "big.txt").string(), false, a, err) && err.find("limit") != std::string::npos);
        { std::ofstream bin(ad / "blob.bin", std::ios::binary); bin << std::string("abc\0def", 7); }
        CHECK(!ai::LoadAttachment((ad / "blob.bin").string(), false, a, err) && err.find("not a text file") != std::string::npos);
        CHECK(!ai::LoadAttachment((ad / "missing.txt").string(), false, a, err));
        CHECK(!ai::LoadAttachment((ad / "notes.cpp").string(), true, a, err));         // a source file is not an image

        // Images: a small PNG goes as it is, a big one is scaled to the limit, a JPEG keeps its type.
        Image small = GenImageColor(64, 48, RED);
        ExportImage(small, (ad / "small.png").string().c_str());
        ExportImage(small, (ad / "small.jpg").string().c_str());
        UnloadImage(small);
        std::string bytes; { std::ifstream in(ad / "small.png", std::ios::binary); std::stringstream ss; ss << in.rdbuf(); bytes = ss.str(); }
        CHECK(ai::LoadAttachment((ad / "small.png").string(), true, a, err) && a.image && a.mime == "image/png" && a.width == 64 && a.height == 48);
        CHECK(a.data == ai::Base64Encode(bytes));                                      // sent unchanged
        CHECK(a.thumbW > 0 && a.thumbW <= 48 && a.thumbH > 0 && a.thumb.size() == (size_t)a.thumbW * a.thumbH * 4);
        CHECK(ai::LoadAttachment((ad / "small.jpg").string(), true, a, err) && a.mime == "image/jpeg" && a.width == 64);
        Image large = GenImageColor(2000, 1200, BLUE);
        ExportImage(large, (ad / "large.png").string().c_str());
        UnloadImage(large);
        CHECK(ai::LoadAttachment((ad / "large.png").string(), true, a, err) && a.mime == "image/png" && a.width == ai::kMaxImageSide && a.height > 900 && a.height < 960);
        CHECK(!ai::LoadAttachment((ad / "large.png").string(), false, a, err) || a.data.find("\x89PNG") == 0);   // as text it is just bytes: a NUL stops it, or it is text
        CHECK(!ai::LoadAttachment((ad / "nothing.png").string(), true, a, err));

        // What the model is sent.
        ai::Attachment txt; txt.name = "a.txt"; txt.data = "hello";
        ai::Attachment img; img.name = "p.png"; img.image = true; img.mime = "image/png"; img.data = "QUJD"; img.width = 1; img.height = 1;
        ai::Attachment gone = img; gone.name = "old.png"; gone.data.clear();
        const std::string composed = ai::ComposeUserText("look", { txt, img, gone });
        CHECK(composed.find("look") == 0 && composed.find("[Attached file: a.txt]\n```\nhello\n```") != std::string::npos);
        CHECK(composed.find("old.png was attached here but is no longer available") != std::string::npos && composed.find("p.png") == std::string::npos);

        ai::ChatRequest pr; pr.model = "m";
        ai::ChatMessage withImage; withImage.user = true; withImage.text = "what is this?"; withImage.images = { img };
        pr.messages = { withImage, { false, "an answer", {} } };
        const auto cj = nlohmann::json::parse(ai::BuildClaudeRequestJson(pr));
        CHECK(cj["messages"][0]["content"].is_array() && cj["messages"][0]["content"][0]["type"] == "image");
        CHECK(cj["messages"][0]["content"][0]["source"]["media_type"] == "image/png" && cj["messages"][0]["content"][0]["source"]["data"] == "QUJD");
        CHECK(cj["messages"][0]["content"][1]["text"] == "what is this?" && cj["messages"][1]["content"] == "an answer");
        const auto oj = nlohmann::json::parse(ai::BuildOpenAIRequestJson(pr));
        CHECK(oj["messages"][0]["content"][0]["text"] == "what is this?" && oj["messages"][0]["content"][1]["image_url"]["url"] == "data:image/png;base64,QUJD");
        const auto gj = nlohmann::json::parse(ai::BuildGeminiRequestJson(pr));
        CHECK(gj["contents"][0]["parts"][0]["text"] == "what is this?" && gj["contents"][0]["parts"][1]["inline_data"]["mime_type"] == "image/png" && gj["contents"][0]["parts"][1]["inline_data"]["data"] == "QUJD");
        CHECK(gj["contents"][1]["parts"].size() == 1);                                // a plain turn has no image parts

        // Saved with the chat: a file keeps its text, an image keeps only its name and size.
        const fs::path cd = dir / "chats_att";
        ai::SetChatsDirForTesting(cd.string());
        ai::Message um; um.user = true; um.text = "see these"; um.attachments = { txt, img };
        CHECK(ai::SaveChatFile({ "c900", "With files", 1 }, { um, { false, false, "ok" } }));
        ai::ChatInfo info; std::vector<ai::Message> back;
        CHECK(ai::LoadChatFile("c900", info, back) && back.size() == 2 && back[0].attachments.size() == 2);
        if (back.size() == 2 && back[0].attachments.size() == 2) {
            CHECK(!back[0].attachments[0].image && back[0].attachments[0].name == "a.txt" && back[0].attachments[0].data == "hello");
            CHECK(back[0].attachments[1].image && back[0].attachments[1].name == "p.png" && back[0].attachments[1].data.empty() && back[0].attachments[1].width == 1);
            CHECK(ai::ComposeUserText(back[0].text, back[0].attachments).find("p.png was attached here but is no longer available") != std::string::npos);
        }
        ai::SetChatsDirForTesting((dir / "chats_default").string());

        // Send puts them in the request: the text file inline, the image as an image part, nothing for a blank message with nothing attached.
        struct Spy final : ai::IProvider {
            ai::ChatRequest* seen;
            explicit Spy(ai::ChatRequest* s) : seen(s) {}
            const char* Name() const override { return "Spy"; }
            ai::ModelListResult ListModels(const std::string&) override { return {}; }
            ai::ChatResult Chat(const ai::ChatRequest& r, const std::string&) override { *seen = r; return { true, "seen", {} }; }
        };
        ai::ChatRequest seen;
        ai::SetProviderFactoryForTesting([&](ai::ProviderId) { return std::unique_ptr<ai::IProvider>(new Spy(&seen)); });
        ai::GetSettings().enabled = true; ai::GetSettings().provider = ai::ProviderId::Claude; ai::GetSettings().key[0] = "k";
        ai::ClearHistory();
        ai::Send("  ", "ctx");                                                      // blank and nothing attached: ignored
        CHECK(ai::History().empty() && !ai::Busy());
        ai::Send("explain these", "ctx", { txt, img });
        for (int i = 0; i < 300 && ai::Busy(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); ai::Update(); }
        CHECK(!ai::Busy() && seen.messages.size() == 1 && seen.messages[0].images.size() == 1 && seen.messages[0].images[0].name == "p.png");
        CHECK(seen.messages[0].text.find("explain these") == 0 && seen.messages[0].text.find("[Attached file: a.txt]") != std::string::npos);
        CHECK(ai::History().size() == 2 && ai::History()[0].text == "explain these" && ai::History()[0].attachments.size() == 2);   // the chat shows what you typed, not the file body
        ai::Send("", "ctx", { txt });                                              // attachments alone are enough
        for (int i = 0; i < 300 && ai::Busy(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); ai::Update(); }
        CHECK(ai::History().size() == 4 && ai::History()[2].text == "Please look at what I attached.");
        ai::SetProviderFactoryForTesting(nullptr);
        ai::ClearHistory();
        ai::GetSettings().enabled = false;
    }

    // Stop: the request is abandoned at once, the transfer is told to abort, and its late result never shows up.
    {
        struct Fake final : ai::IProvider {
            std::atomic<int>* calls; std::atomic<bool>* sawCancel;
            Fake(std::atomic<int>* c, std::atomic<bool>* s) : calls(c), sawCancel(s) {}
            const char* Name() const override { return "Fake"; }
            ai::ModelListResult ListModels(const std::string&) override { return {}; }
            ai::ChatResult Chat(const ai::ChatRequest& req, const std::string&) override {
                const int n = ++*calls;
                if (n == 1) {                                          // the first call hangs until it is told to stop
                    for (int i = 0; i < 300 && !req.cancel->load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    *sawCancel = req.cancel->load();
                    return { true, "late reply that must be ignored", {} };
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                return { true, "second answer", {} };
            }
        };
        std::atomic<int> calls{ 0 };
        std::atomic<bool> sawCancel{ false };
        ai::SetProviderFactoryForTesting([&](ai::ProviderId) { return std::unique_ptr<ai::IProvider>(new Fake(&calls, &sawCancel)); });
        ai::GetSettings().enabled = true; ai::GetSettings().provider = ai::ProviderId::Claude; ai::GetSettings().key[0] = "k";
        ai::ClearHistory();
        auto wait = [](auto pred, int ms) { for (int i = 0; i < ms / 10 && !pred(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); ai::Update(); } };

        ai::Stop();                                                    // nothing running: nothing happens
        CHECK(ai::History().empty());
        ai::Send("hello", "");
        CHECK(ai::Busy() && ai::History().size() == 1);
        ai::Stop();
        CHECK(!ai::Busy());                                            // free immediately, no waiting for the thread
        CHECK(ai::History().size() == 2 && ai::History().back().error && ai::History().back().stopped && ai::History().back().text == "Stopped.");
        wait([&] { return sawCancel.load(); }, 2000);
        CHECK(sawCancel.load());                                       // the worker saw the cancel flag
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ai::Update();
        CHECK(!ai::Busy() && ai::History().size() == 2);               // the late reply was dropped

        ai::Send("second question", "");                               // a new request works, unaffected by the old one
        CHECK(ai::Busy());
        wait([] { return !ai::Busy(); }, 3000);
        CHECK(!ai::Busy() && ai::History().size() == 4 && ai::History().back().text == "second answer" && !ai::History().back().error);

        CHECK(ai::RetryLast("") == false);                             // last message is a reply, not an error: nothing to retry
        ai::SetProviderFactoryForTesting(nullptr);
        ai::ClearHistory();
        ai::GetSettings().enabled = false;
    }

    // What Apply may do.
    CHECK(ai::IsSafeScriptName("Spin.cpp") && ai::IsSafeScriptName("_a1.cpp"));
    CHECK(!ai::IsSafeScriptName("../x.cpp") && !ai::IsSafeScriptName("a/b.cpp") && !ai::IsSafeScriptName("a\\b.cpp"));
    CHECK(!ai::IsSafeScriptName("x.h") && !ai::IsSafeScriptName(".cpp") && !ai::IsSafeScriptName("1a.cpp") && !ai::IsSafeScriptName("a b.cpp"));
    CHECK(ai::IsAllowedConsoleLine("Cube.Position = (0,0,0)"));
    CHECK(!ai::IsAllowedConsoleLine("fcloud change --apikey x") && !ai::IsAllowedConsoleLine("  fcloud push"));


    // Saved chats: one owner-only JSON file each, newest first, ids can never escape the folder.
    const fs::path chats = dir / "chats";
    ai::SetChatsDirForTesting(chats.string());
    CHECK(ai::ListChats().empty());
    CHECK(ai::IsValidChatId("c1700000000000") && !ai::IsValidChatId("../x") && !ai::IsValidChatId("A") && !ai::IsValidChatId("") && !ai::IsValidChatId("a/b"));
    std::vector<ai::Message> msgs = { { true, false, "make a cube \"quoted\"\nline2" }, { false, false, "Sure **bold**" }, { false, true, "an error notice" } };
    CHECK(ai::SaveChatFile({ "c100", "First chat", 100 }, msgs));
    CHECK(ai::SaveChatFile({ "c200", "Second chat", 200 }, msgs));
    CHECK(!ai::SaveChatFile({ "../evil", "x", 1 }, msgs));
#if !defined(_WIN32)
    CHECK((fs::status(chats / "c100.json").permissions() & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
#endif
    CHECK(!fs::exists(chats / "c100.json.tmp"));
    {
        const auto list = ai::ListChats();
        CHECK(list.size() == 2 && list[0].id == "c200" && list[1].title == "First chat");
        ai::ChatInfo info; std::vector<ai::Message> back;
        CHECK(ai::LoadChatFile("c100", info, back));
        CHECK(back.size() == 2);                                  // the error notice was not saved
        if (back.size() == 2) CHECK(back[0].user && back[0].text == msgs[0].text && !back[1].user && back[1].text == "Sure **bold**");
        CHECK(!ai::LoadChatFile("nope", info, back));
    }
    { std::ofstream(chats / "c300.json") << "{ not json"; CHECK(ai::ListChats().size() == 2); }   // a damaged file is skipped
    CHECK(ai::OpenChat("c100") && ai::CurrentChatId() == "c100" && ai::CurrentChatTitle() == "First chat" && ai::History().size() == 2);
    ai::DeleteChat("c200");
    CHECK(ai::ListChats().size() == 1 && ai::CurrentChatId() == "c100");
    ai::DeleteChat("c100");                                       // deleting the open chat starts a new one
    CHECK(ai::CurrentChatId().empty() && ai::History().empty() && ai::ListChats().empty());
    ai::DeleteChat("../x");                                       // refused, nothing happens
    ai::SetChatsDirForTesting("");

    // The system prompt carries the embedded docs.
    const std::string sp = ai::EngineSystemPrompt();
    CHECK(sp.find("fly-console") != std::string::npos && sp.size() > 20000);

    fs::remove_all(dir);
    ai::SetConfigPathForTesting("");
    std::printf(g_fail ? "ai_test: %d failure(s)\n" : "ai_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
