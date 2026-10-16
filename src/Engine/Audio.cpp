#include "../../include/Engine/Audio.hpp"
#include "../../include/Engine/Vfs.hpp"
#include "../../include/Engine/Backend/audio.h"   // miniaudio; the implementation is in Backend/miniaudio_impl.c

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// stb_vorbis.c (compiled as C, see CMakeLists.txt): decodes a whole OGG Vorbis file. Returns the frames per
// channel, or -1; `*output` is malloc'ed.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

namespace audio {
namespace {

constexpr size_t kMaxVoices = 64;
constexpr uint64_t kMaxClipSamples = 128ull * 1024 * 1024;   // 512 MB of float PCM, per clip

struct Clip {
    std::vector<float> pcm;      // interleaved
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t frames = 0;
};

struct Voice {
    Handle id = 0;
    std::shared_ptr<Clip> clip;
    ma_audio_buffer buffer{};    // a private read cursor over the clip's shared PCM
    ma_sound sound{};
    bool bufferInit = false;
    bool soundInit = false;
    bool spatial = false;
    ~Voice() {
        if (soundInit) ma_sound_uninit(&sound);
        if (bufferInit) ma_audio_buffer_uninit(&buffer);
    }
};

struct State {
    std::mutex mutex;
    ma_engine engine{};
    bool open = false;
    bool failed = false;         // the device couldn't be opened; don't retry every Play
    bool noDevice = false;
    float master = 1.0f;
    Handle nextId = 1;
    std::vector<std::unique_ptr<Voice>> voices;
    std::unordered_map<std::string, std::shared_ptr<Clip>> clips;
};

State& S() { static State s; return s; }

std::function<void(const std::string&)>& LogHandler() { static std::function<void(const std::string&)> h; return h; }
void Log(const std::string& msg) {
    if (auto& h = LogHandler()) h(msg);
    else std::fprintf(stderr, "%s\n", msg.c_str());
}
std::set<std::string> g_warned;   // a file that can't be played is reported once, not on every attempt

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    if (vfs::Handles(path)) return vfs::Read(path, out);
    std::ifstream in(path, std::ios::binary);
    if (in) {
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return !out.empty();
    }
    return vfs::Active() && vfs::Read(path, out);   // an exported game names its files relative to the archive
}

std::shared_ptr<Clip> Decode(const std::vector<uint8_t>& bytes) {
    auto clip = std::make_shared<Clip>();
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
        int channels = 0, rate = 0;
        short* samples = nullptr;
        const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &samples);
        if (frames <= 0 || !samples || channels <= 0) { std::free(samples); return nullptr; }
        const uint64_t total = static_cast<uint64_t>(frames) * static_cast<uint64_t>(channels);
        if (total > kMaxClipSamples) { std::free(samples); return nullptr; }
        clip->pcm.resize(static_cast<size_t>(total));
        for (uint64_t i = 0; i < total; ++i) clip->pcm[static_cast<size_t>(i)] = static_cast<float>(samples[i]) / 32768.0f;
        std::free(samples);
        clip->channels = static_cast<uint32_t>(channels);
        clip->sampleRate = static_cast<uint32_t>(rate);
        clip->frames = static_cast<uint64_t>(frames);
        return clip;
    }

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);   // native channels and rate; the mixer converts
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) return nullptr;
    clip->channels = dec.outputChannels;
    clip->sampleRate = dec.outputSampleRate;
    if (clip->channels == 0 || clip->sampleRate == 0) { ma_decoder_uninit(&dec); return nullptr; }

    // The length isn't always known up front (MP3), so read in chunks until the stream runs dry.
    constexpr ma_uint64 kChunk = 16384;
    std::vector<float> chunk(static_cast<size_t>(kChunk) * clip->channels);
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&dec, chunk.data(), kChunk, &got);
        if (got > 0) {
            if ((clip->pcm.size() + got * clip->channels) > kMaxClipSamples) { clip.reset(); break; }
            clip->pcm.insert(clip->pcm.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got * clip->channels));
        }
        if (r != MA_SUCCESS || got < kChunk) break;
    }
    ma_decoder_uninit(&dec);
    if (!clip || clip->pcm.empty()) return nullptr;
    clip->frames = clip->pcm.size() / clip->channels;
    return clip;
}

std::shared_ptr<Clip> LoadClipLocked(State& s, const std::string& path) {
    if (auto it = s.clips.find(path); it != s.clips.end()) return it->second;
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        if (g_warned.insert(path).second) Log("[audio] can't find sound file: " + path);
        return nullptr;
    }
    auto clip = Decode(bytes);
    if (clip) s.clips[path] = clip;
    else if (g_warned.insert(path).second) Log("[audio] can't decode " + path + " (not a valid WAV, MP3, FLAC or OGG file?)");
    return clip;
}

void ReapLocked(State& s) {
    s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(), [](const std::unique_ptr<Voice>& v) {
        return ma_sound_at_end(&v->sound) == MA_TRUE;
    }), s.voices.end());
}

Voice* FindLocked(State& s, Handle h) {
    for (auto& v : s.voices) if (v->id == h) return v.get();
    return nullptr;
}

Handle StartLocked(State& s, const std::string& path, const PlayParams& p, bool spatial, float x, float y, float z) {
    if (!s.open) return 0;
    auto clip = LoadClipLocked(s, path);
    if (!clip) return 0;
    ReapLocked(s);
    if (s.voices.size() >= kMaxVoices) s.voices.erase(s.voices.begin());   // the oldest gives way

    auto v = std::make_unique<Voice>();
    v->clip = clip;
    v->spatial = spatial;
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, clip->channels, clip->frames, clip->pcm.data(), nullptr);
    bc.sampleRate = clip->sampleRate;
    if (ma_audio_buffer_init(&bc, &v->buffer) != MA_SUCCESS) return 0;
    v->bufferInit = true;
    const ma_uint32 flags = spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_data_source(&s.engine, &v->buffer, flags, nullptr, &v->sound) != MA_SUCCESS) return 0;
    v->soundInit = true;

    ma_sound_set_volume(&v->sound, std::max(0.0f, p.volume));
    ma_sound_set_pitch(&v->sound, std::clamp(p.pitch, 0.05f, 8.0f));
    if (!spatial) ma_sound_set_pan(&v->sound, std::clamp(p.pan, -1.0f, 1.0f));
    ma_sound_set_looping(&v->sound, p.loop ? MA_TRUE : MA_FALSE);
    if (spatial) ma_sound_set_position(&v->sound, x, y, z);
    if (ma_sound_start(&v->sound) != MA_SUCCESS) return 0;

    v->id = s.nextId++;
    if (s.nextId == 0) s.nextId = 1;
    const Handle id = v->id;
    s.voices.push_back(std::move(v));
    return id;
}

} // namespace

void SetLogHandler(std::function<void(const std::string&)> handler) { LogHandler() = std::move(handler); }

bool Init(bool noDevice) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.open) return true;
    if (s.failed) return false;
    ma_engine_config cfg = ma_engine_config_init();
    if (noDevice) { cfg.noDevice = MA_TRUE; cfg.channels = 2; cfg.sampleRate = 48000; }
    const ma_result initResult = ma_engine_init(&cfg, &s.engine);
    if (initResult != MA_SUCCESS) {
        s.failed = true;
        Log(std::string("[audio] no sound device, sounds will not play (") + ma_result_description(initResult) + ")");
        return false;
    }
    s.open = true;
    if (!noDevice) {
        Log("[audio] sound device ready: " + std::to_string(ma_engine_get_sample_rate(&s.engine)) + " Hz, " +
            std::to_string(ma_engine_get_channels(&s.engine)) + " channels");
    }
    s.noDevice = noDevice;
    ma_engine_set_volume(&s.engine, s.master);
    return true;
}

void Shutdown() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.voices.clear();
    s.clips.clear();
    g_warned.clear();
    if (s.open) ma_engine_uninit(&s.engine);
    s.open = false;
    s.failed = false;
}

bool Available() { State& s = S(); std::lock_guard<std::mutex> lock(s.mutex); return s.open; }

Handle Play(const std::string& path, const PlayParams& params) {
    if (!Init()) return 0;
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return StartLocked(s, path, params, false, 0, 0, 0);
}

Handle PlayAt(const std::string& path, float x, float y, float z, const PlayParams& params) {
    if (!Init()) return 0;
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return StartLocked(s, path, params, true, x, y, z);
}

void Stop(Handle h) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(),
                   [h](const std::unique_ptr<Voice>& v) { return v->id == h; }), s.voices.end());
}

void StopAll() { State& s = S(); std::lock_guard<std::mutex> lock(s.mutex); s.voices.clear(); }

bool IsPlaying(Handle h) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Voice* v = FindLocked(s, h);
    return v && ma_sound_at_end(&v->sound) == MA_FALSE;
}

void SetPosition(Handle h, float x, float y, float z) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Voice* v = FindLocked(s, h); v && v->spatial) ma_sound_set_position(&v->sound, x, y, z);
}

void SetVolume(Handle h, float volume) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Voice* v = FindLocked(s, h)) ma_sound_set_volume(&v->sound, std::max(0.0f, volume));
}

void SetMasterVolume(float volume) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.master = std::clamp(volume, 0.0f, 1.0f);
    if (s.open) ma_engine_set_volume(&s.engine, s.master);
}

float MasterVolume() { State& s = S(); std::lock_guard<std::mutex> lock(s.mutex); return s.master; }

void SetListener(float px, float py, float pz, float fx, float fy, float fz, float ux, float uy, float uz) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.open) return;
    ma_engine_listener_set_position(&s.engine, 0, px, py, pz);
    ma_engine_listener_set_direction(&s.engine, 0, fx, fy, fz);
    ma_engine_listener_set_world_up(&s.engine, 0, ux, uy, uz);
}

void Update() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.open) ReapLocked(s);
}

bool Preload(const std::string& path) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return LoadClipLocked(s, path) != nullptr;
}

bool GetClipInfo(const std::string& path, ClipInfo& out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    auto clip = LoadClipLocked(s, path);
    if (!clip) return false;
    out = { clip->frames, clip->channels, clip->sampleRate };
    return true;
}

void ClearCache() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.voices.clear();
    s.clips.clear();
}

int ActiveVoices() { State& s = S(); std::lock_guard<std::mutex> lock(s.mutex); return static_cast<int>(s.voices.size()); }

uint32_t Pump(uint32_t frames, float* out) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.open || !s.noDevice) return 0;
    std::vector<float> scratch;
    if (!out) { scratch.resize(static_cast<size_t>(frames) * 2); out = scratch.data(); }
    ma_uint64 got = 0;
    if (ma_engine_read_pcm_frames(&s.engine, out, frames, &got) != MA_SUCCESS) return static_cast<uint32_t>(got);
    return static_cast<uint32_t>(got);
}

} // namespace audio
