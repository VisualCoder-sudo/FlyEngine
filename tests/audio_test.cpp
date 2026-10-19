// The sound system (include/Engine/Audio.hpp) without a sound card: decode a WAV, mix it through a device-less
// engine and look at the samples. No window.
#include "Engine/Audio.hpp"
#include "Engine/PackArchive.hpp"
#include "Engine/Vfs.hpp"

#include <chrono>
#include <cmath>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

namespace fs = std::filesystem;

// A 16-bit mono PCM WAV: a sine at `hz`, `seconds` long.
static std::string WriteSine(const fs::path& path, uint32_t rate, float seconds, float hz) {
    const uint32_t frames = static_cast<uint32_t>(rate * seconds);
    std::vector<int16_t> pcm(frames);
    for (uint32_t i = 0; i < frames; ++i)
        pcm[i] = static_cast<int16_t>(std::sin(6.2831853f * hz * static_cast<float>(i) / static_cast<float>(rate)) * 30000.0f);
    const uint32_t dataBytes = frames * 2, riff = 36 + dataBytes, fmtSize = 16, byteRate = rate * 2;
    const uint16_t pcmTag = 1, channels = 1, align = 2, bits = 16;
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4); out.write(reinterpret_cast<const char*>(&riff), 4); out.write("WAVEfmt ", 8);
    out.write(reinterpret_cast<const char*>(&fmtSize), 4); out.write(reinterpret_cast<const char*>(&pcmTag), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2); out.write(reinterpret_cast<const char*>(&rate), 4);
    out.write(reinterpret_cast<const char*>(&byteRate), 4); out.write(reinterpret_cast<const char*>(&align), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2); out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&dataBytes), 4);
    out.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
    return path.string();
}

// Mixes `frames` frames and returns the peak of the left and right channels.
static void Peaks(uint32_t frames, float& left, float& right) {
    std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
    const uint32_t got = audio::Pump(frames, buf.data());
    left = right = 0.0f;
    for (uint32_t i = 0; i < got; ++i) {
        left = std::fmax(left, std::fabs(buf[i * 2]));
        right = std::fmax(right, std::fabs(buf[i * 2 + 1]));
    }
}

// `audio_test --device`: plays a short tone through the real sound device, to check that this machine can make sound.
static int PlayOnDevice() {
    const fs::path dir = fs::temp_directory_path() / "fly_audio_device";
    fs::create_directories(dir);
    const std::string wav = WriteSine(dir / "tone.wav", 44100, 1.0f, 440.0f);
    if (!audio::Init()) { std::printf("no sound device\n"); return 1; }
    audio::PlayParams p; p.volume = 0.3f;
    const audio::Handle h = audio::Play(wav, p);
    std::printf("playing a 1 s 440 Hz tone: handle %u, playing %d\n", h, audio::IsPlaying(h) ? 1 : 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    audio::Update();
    std::printf("finished: %s\n", audio::IsPlaying(h) ? "no" : "yes");
    audio::Shutdown();
    std::error_code ec; fs::remove_all(dir, ec);
    return h != 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--device") == 0) return PlayOnDevice();
    const fs::path dir = fs::temp_directory_path() / "fly_audio_test";
    fs::create_directories(dir);
    const std::string wav = WriteSine(dir / "tone.wav", 8000, 0.5f, 440.0f);

    // Without an open sound system everything is a quiet no-op.
    CHECK(!audio::Available());
    CHECK(audio::Pump(64) == 0);

    audio::ClipInfo info;
    CHECK(audio::GetClipInfo(wav, info));
    CHECK(info.frames == 4000 && info.channels == 1 && info.sampleRate == 8000);
    CHECK(!audio::GetClipInfo((dir / "missing.wav").string(), info));
    CHECK(!audio::Preload((dir / "missing.wav").string()));

    CHECK(audio::Init(/*noDevice=*/true));
    CHECK(audio::Available());
    CHECK(audio::Play((dir / "missing.wav").string()) == 0);

    // A one-shot plays, is heard on both sides, then ends and is freed.
    audio::Handle h = audio::Play(wav);
    CHECK(h != 0);
    CHECK(audio::IsPlaying(h));
    CHECK(audio::ActiveVoices() == 1);
    float l = 0, r = 0;
    Peaks(4800, l, r);                               // 0.1 s
    CHECK(l > 0.1f && r > 0.1f);
    Peaks(48000, l, r);                              // well past the clip's 0.5 s
    audio::Update();
    CHECK(!audio::IsPlaying(h));
    CHECK(audio::ActiveVoices() == 0);
    Peaks(4800, l, r);
    CHECK(l == 0.0f && r == 0.0f);                   // nothing left to hear

    // Volume, master volume.
    audio::PlayParams quiet; quiet.volume = 0.25f;
    audio::Play(wav);
    float fullL, fullR; Peaks(2400, fullL, fullR);
    audio::StopAll();
    audio::Play(wav, quiet);
    float lowL, lowR; Peaks(2400, lowL, lowR);
    CHECK(lowL < fullL * 0.5f && lowL > 0.0f);
    audio::StopAll();
    audio::SetMasterVolume(0.0f);
    audio::Play(wav);
    Peaks(2400, l, r);
    CHECK(l == 0.0f && r == 0.0f);
    audio::SetMasterVolume(1.0f);
    CHECK(audio::MasterVolume() == 1.0f);
    audio::StopAll();

    // Looping keeps going past the clip's end; Stop ends it.
    audio::PlayParams loop; loop.loop = true;
    h = audio::Play(wav, loop);
    Peaks(48000, l, r);
    audio::Update();
    CHECK(audio::IsPlaying(h));
    Peaks(2400, l, r);
    CHECK(l > 0.1f);
    audio::Stop(h);
    CHECK(!audio::IsPlaying(h) && audio::ActiveVoices() == 0);

    // A sound in the world is louder on the side it is on.
    audio::SetListener(0, 0, 0, 0, 0, 1, 0, 1, 0);   // at the origin, facing +Z
    audio::PlayAt(wav, 4.0f, 0.0f, 0.0f);
    Peaks(2400, l, r);
    audio::StopAll();
    const float rightSide = r - l;   // (negative: left is louder)
    audio::PlayAt(wav, -4.0f, 0.0f, 0.0f);
    Peaks(2400, l, r);
    audio::StopAll();
    const float leftSide = l - r;    // (negative: right is louder)
    // The world is right-handed (like the camera): facing +Z, +X is on the listener's left.
    CHECK(rightSide < -0.05f);                       // the +X sound is louder in the left ear
    CHECK(leftSide < -0.05f);                        // the -X sound is louder in the right ear
    // Sound rays' effects: a low-pass filter (muffle), an echo, and a range that fades to silence.
    {
        audio::StopAll();
        audio::SetListener(0, 0, 0, 0, 0, 1, 0, 1, 0);
        auto rms = [](uint32_t frames) {
            std::vector<float> buf(static_cast<size_t>(frames) * 2, 0.0f);
            const uint32_t got = audio::Pump(frames, buf.data());
            double sum = 0.0;
            for (uint32_t i = 0; i < got * 2; ++i) sum += (double)buf[i] * buf[i];
            return got ? static_cast<float>(std::sqrt(sum / (got * 2))) : 0.0f;
        };
        const std::string hi = WriteSine(dir / "hi.wav", 48000, 1.0f, 8000.0f);   // a high tone: what a wall takes away

        audio::PlayParams open; open.maxDistance = 50.0f;
        audio::PlayAt(hi, 0, 0, 3, open);
        const float openRms = rms(4800);
        audio::StopAll();
        audio::PlayParams muffled = open; muffled.muffle = true; muffled.muffleCutoff = 500.0f;
        audio::PlayAt(hi, 0, 0, 3, muffled);
        const float mufRms = rms(4800);
        audio::StopAll();
        CHECK(openRms > 0.02f && mufRms < openRms * 0.2f);                    // muffled behind a wall

        audio::PlayParams live = open; live.muffle = true;                    // and it can change while playing
        const audio::Handle lh = audio::PlayAt(hi, 0, 0, 3, live);
        const float before = rms(4800);
        audio::SetMuffle(lh, 500.0f, 1.0f);
        rms(4800);                                                            // let the filter settle
        const float after = rms(4800);
        CHECK(before > 0.02f && after < before * 0.2f);
        audio::StopAll();

        // Range: full volume near, quieter farther, silent past the range.
        audio::PlayParams ranged; ranged.maxDistance = 10.0f; ranged.minDistance = 1.0f;
        audio::PlayAt(hi, 0, 0, 1, ranged);   const float near1 = rms(2400);   audio::StopAll();
        audio::PlayAt(hi, 0, 0, 5, ranged);   const float mid5 = rms(2400);    audio::StopAll();
        audio::PlayAt(hi, 0, 0, 12, ranged);  const float far12 = rms(2400);   audio::StopAll();
        CHECK(near1 > mid5 && mid5 > 0.0f && far12 < 0.001f);

        // Echo: after a short clip ends, its delayed copy is still heard; without echo there is silence.
        const std::string blip = WriteSine(dir / "blip.wav", 48000, 0.2f, 440.0f);
        audio::PlayParams plain = open;
        audio::PlayAt(blip, 0, 0, 2, plain);
        rms(10080);                                                           // 0.21 s: the clip is over
        const float dry = rms(2880);
        audio::StopAll();
        audio::PlayParams echo = open; echo.echoDelay = 0.1f; echo.echoWet = 0.8f; echo.echoFeedback = 0.5f;
        const audio::Handle eh = audio::PlayAt(blip, 0, 0, 2, echo);
        CHECK(eh != 0 && audio::HasEcho(eh));
        rms(10080);
        const float wet = rms(2880);
        CHECK(dry < 0.0005f && wet > 0.01f);                                  // the echo is there, the plain sound is gone
        audio::Update();
        CHECK(audio::ActiveVoices() == 1);                                    // the delay line is kept while it rings
        for (int i = 0; i < 12; ++i) { rms(24000); audio::Update(); }         // 6 s later
        CHECK(audio::ActiveVoices() == 0);                                    // and let go afterwards
        audio::StopAll();

        // Only so many sounds get an echo chain (it costs memory).
        int withEcho = 0;
        for (int i = 0; i < 20; ++i) { const audio::Handle h = audio::PlayAt(blip, 0, 0, 2, echo); if (h && audio::HasEcho(h)) ++withEcho; }
        CHECK(withEcho > 0 && withEcho <= 16);
        audio::StopAll();
        audio::StopByPath(blip);                                              // safe with nothing playing
    }

    // Turning on the spot: a sound 2 m away stays about as loud whichever way you face; only its left/right balance moves (it is
    // louder in the ear on its side, never silent in the other).
    {
        audio::StopAll();
        const std::string tone = WriteSine(dir / "turn.wav", 48000, 2.0f, 300.0f);
        float quietest = 1e9f, loudest = 0.0f;
        for (int deg = 0; deg < 360; deg += 30) {
            audio::StopAll();
            const float a = deg * 3.14159265f / 180.0f;
            audio::SetListener(0, 0, 0, std::sin(a), 0, std::cos(a), 0, 1, 0);   // yaw `deg` from facing +Z
            audio::PlayParams p; p.loop = true; p.maxDistance = 50.0f;
            audio::PlayAt(tone, 2.0f, 0, 0, p);                                    // 2 m to the +X side
            float lpk = 0, rpk = 0; Peaks(9600, lpk, rpk);
            const float power = std::sqrt(lpk * lpk + rpk * rpk);
            quietest = std::fmin(quietest, power); loudest = std::fmax(loudest, power);
            CHECK(lpk > 0.35f * rpk && rpk > 0.35f * lpk);                          // both ears always hear it
        }
        std::printf("turning on the spot: loudest %.3f, quietest %.3f\n", loudest, quietest);
        CHECK(quietest > loudest * 0.85f);
        audio::StopAll();
        audio::SetListener(0, 0, 0, 0, 0, 1, 0, 1, 0);   // facing +Z: +X is on the left (right-handed world)
        audio::PlayParams p; p.loop = true; p.maxDistance = 50.0f;
        audio::PlayAt(tone, 2.0f, 0, 0, p);
        float lpk = 0, rpk = 0; Peaks(9600, lpk, rpk);
        CHECK(lpk > rpk * 1.5f);
        audio::StopAll();
    }

    // More sounds than voices: the oldest give way, the count stays capped.
    audio::StopAll();
    audio::PlayParams longLoop; longLoop.loop = true;   // loops never end, so only the cap can remove them
    const audio::Handle first = audio::Play(wav, longLoop);
    for (int i = 0; i < 99; ++i) audio::Play(wav, longLoop);
    CHECK(audio::ActiveVoices() == 64);
    CHECK(!audio::IsPlaying(first));                    // the oldest gave way
    audio::StopAll();

    // ClearCache stops everything and forgets decoded clips; a clip still loads again afterwards.
    audio::Play(wav, longLoop);
    audio::ClearCache();
    CHECK(audio::ActiveVoices() == 0);
    CHECK(audio::GetClipInfo(wav, info) && info.frames == 4000);

    // A file that is not audio fails to decode, and does not break later loads.
    {
        const fs::path junk = dir / "junk.wav";
        std::ofstream(junk, std::ios::binary) << "this is not audio";
        CHECK(!audio::Preload(junk.string()));
        CHECK(audio::Play(junk.string()) == 0);
        CHECK(audio::Preload(wav));
    }

    // An exported game: the sound is inside the archive appended to the executable, named relative to it or
    // under the virtual root, and plays from there.
    {
        const fs::path fakeExe = dir / "player", packed = dir / "game";
        std::ofstream(fakeExe, std::ios::binary) << "not really an executable";
        pack::Entry e; e.path = "assets/Audio/tone.wav"; e.diskPath = wav;
        std::string err;
        CHECK(pack::BuildExecutable(fakeExe.string(), packed.string(), { e }, err));
        CHECK(vfs::MountFile(packed.string()));
        audio::ClipInfo packedInfo;
        CHECK(audio::GetClipInfo(vfs::VirtualPath("assets/Audio/tone.wav"), packedInfo));
        CHECK(packedInfo.frames == 4000 && packedInfo.sampleRate == 8000);
        CHECK(audio::GetClipInfo("assets/Audio/tone.wav", packedInfo) && packedInfo.frames == 4000);
        CHECK(audio::Play(vfs::VirtualPath("assets/Audio/tone.wav")) != 0);
        float pl, pr; Peaks(2400, pl, pr);
        CHECK(pl > 0.1f);
        audio::StopAll();
        vfs::Unmount();
    }

    audio::Shutdown();
    CHECK(!audio::Available());
    std::error_code ec; fs::remove_all(dir, ec);

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
