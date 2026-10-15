// The sound system (include/Engine/Audio.hpp) without a sound card: decode a WAV, mix it through a device-less
// engine and look at the samples. No window.
#include "Engine/Audio.hpp"
#include "Engine/PackArchive.hpp"
#include "Engine/Vfs.hpp"

#include <cmath>
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

int main() {
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
    // More sounds than voices: the oldest give way, the count stays capped.
    for (int i = 0; i < 100; ++i) audio::Play(wav);
    CHECK(audio::ActiveVoices() <= 64);

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
