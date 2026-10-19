#include "../../include/Engine/Audio.hpp"
#include "../../include/Engine/Vfs.hpp"
#include "../../include/Engine/Backend/audio.h"   // miniaudio; the implementation is in Backend/miniaudio_impl.c

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <chrono>
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
constexpr size_t kMaxCacheBytes = 768ull * 1024 * 1024;       // decoded clips kept for reuse; unused ones are evicted past this
constexpr int kInitRetrySeconds = 5;                          // after a failed device open, Init tries again no sooner than this

struct Clip {
    std::vector<float> pcm;      // interleaved
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint64_t frames = 0;
    uint64_t lastUse = 0;        // cache tick, for evicting the least recently used
};

constexpr int kMaxEchoVoices = 16;
// Left/right placement of 3D sounds. miniaudio's own panning halves the gain of a sound straight ahead or behind and mutes the far
// ear at the side, so turning on the spot swings a sound between loud and about 6 dB down. That is switched off for our sounds
// (directional attenuation 0) and a gentler balance is applied here: the near ear stays at full volume, the far one drops at most
// to 1 - kMaxPan, and a sound ahead or behind is not quieter at all.
constexpr float kMaxPan = 0.55f;
constexpr float kMuffleCutoffOpen = 22000.0f;

struct Voice {
    Handle id = 0;
    std::string path;
    std::shared_ptr<Clip> clip;
    ma_audio_buffer buffer{};    // a private read cursor over the clip's shared PCM
    ma_sound sound{};
    ma_lpf_node lpf{};           // sound rays: muffle (sound -> lpf -> [delay] -> endpoint)
    ma_delay_node delay{};       // sound rays: echo
    bool bufferInit = false;
    bool soundInit = false;
    bool lpfInit = false;
    bool delayInit = false;
    bool spatial = false;
    float pos[3] = { 0, 0, 0 };  // where a 3D sound is
    float volume = 1.0f;         // the sound's own volume; the muffle gain multiplies it
    float muffleGain = 1.0f;
    float panGain = 1.0f;        // keeps the loudness the same as the sound moves from ahead to the side (see ApplyPanLocked)
    float cutoff = kMuffleCutoffOpen;
    float echoWet = 0.0f, echoFeedback = 0.0f;
    ma_uint32 delayFrames = 0;
    ma_uint64 tailEndFrame = 0;  // an echoing sound that has finished keeps its delay line running until here
    ~Voice() {
        if (soundInit) ma_sound_uninit(&sound);   // first: it feeds the nodes
        if (lpfInit) ma_lpf_node_uninit(&lpf, nullptr);
        if (delayInit) ma_delay_node_uninit(&delay, nullptr);
        if (bufferInit) ma_audio_buffer_uninit(&buffer);
    }
};

struct State {
    std::mutex mutex;
    ma_engine engine{};
    bool open = false;
    bool failed = false;         // the device couldn't be opened; don't retry every Play (see kInitRetrySeconds)
    bool failLogged = false;
    std::chrono::steady_clock::time_point failedAt{};
    uint64_t useTick = 0;
    bool noDevice = false;
    float master = 1.0f;
    float listener[3] = { 0, 0, 0 };
    float listenerForward[3] = { 0, 0, -1 };
    float listenerUp[3] = { 0, 1, 0 };
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

std::shared_ptr<Clip> Decode(const std::vector<uint8_t>& bytes, bool& tooLarge) {
    tooLarge = false;
    auto clip = std::make_shared<Clip>();
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "OggS", 4) == 0) {
        int channels = 0, rate = 0;
        short* samples = nullptr;
        const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()), &channels, &rate, &samples);
        if (frames <= 0 || !samples || channels <= 0) { std::free(samples); return nullptr; }
        const uint64_t total = static_cast<uint64_t>(frames) * static_cast<uint64_t>(channels);
        if (total > kMaxClipSamples) { std::free(samples); tooLarge = true; return nullptr; }
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
            if ((clip->pcm.size() + got * clip->channels) > kMaxClipSamples) { clip.reset(); tooLarge = true; break; }
            clip->pcm.insert(clip->pcm.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got * clip->channels));
        }
        if (r != MA_SUCCESS || got < kChunk) break;
    }
    ma_decoder_uninit(&dec);
    if (!clip || clip->pcm.empty()) return nullptr;
    clip->frames = clip->pcm.size() / clip->channels;
    return clip;
}

// Drops the least recently used clips nothing is playing (only the cache holds them) until the cache fits its budget.
void EvictLocked(State& s, const Clip* keep) {
    size_t total = 0;
    for (const auto& [path, c] : s.clips) total += c->pcm.size() * sizeof(float);
    while (total > kMaxCacheBytes) {
        auto victim = s.clips.end();
        for (auto it = s.clips.begin(); it != s.clips.end(); ++it) {
            if (it->second.use_count() != 1 || it->second.get() == keep) continue;
            if (victim == s.clips.end() || it->second->lastUse < victim->second->lastUse) victim = it;
        }
        if (victim == s.clips.end()) return;   // everything left is playing
        total -= victim->second->pcm.size() * sizeof(float);
        s.clips.erase(victim);
    }
}

// Reads and decodes outside the lock (a long clip would otherwise stall every other audio call), then inserts under it.
// Call without holding s.mutex.
std::shared_ptr<Clip> LoadClip(State& s, const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (auto it = s.clips.find(path); it != s.clips.end()) { it->second->lastUse = ++s.useTick; return it->second; }
    }
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (g_warned.insert(path).second) Log("[audio] can't find sound file: " + path);
        return nullptr;
    }
    bool tooLarge = false;
    auto clip = Decode(bytes, tooLarge);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!clip) {
        if (g_warned.insert(path).second) {
            if (tooLarge) Log("[audio] can't load " + path + " (too large: over " + std::to_string(kMaxClipSamples / (1024 * 1024)) + "M samples)");
            else Log("[audio] can't decode " + path + " (not a valid WAV, MP3, FLAC or OGG file?)");
        }
        return nullptr;
    }
    auto [it, inserted] = s.clips.emplace(path, clip);   // another thread may have decoded the same file meanwhile
    it->second->lastUse = ++s.useTick;
    std::shared_ptr<Clip> result = it->second;
    if (inserted) EvictLocked(s, result.get());
    return result;
}

void ReapLocked(State& s) {
    const ma_uint64 now = ma_engine_get_time_in_pcm_frames(&s.engine);
    const float rate = (float)ma_engine_get_sample_rate(&s.engine);
    s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(), [now, rate](const std::unique_ptr<Voice>& v) {
        if (ma_sound_at_end(&v->sound) != MA_TRUE) return false;
        if (!v->delayInit) return true;
        // The sound is over but its echo is still ringing: keep the delay line until the repeats have died away.
        if (v->tailEndFrame == 0) v->tailEndFrame = now + (ma_uint64)(std::min(4.0f, (float)v->delayFrames / rate * 8.0f) * rate) + 1;
        return now >= v->tailEndFrame;
    }), s.voices.end());
}

void ApplyVolume(Voice& v) { ma_sound_set_volume(&v.sound, v.volume * v.muffleGain * v.panGain); }

// Where the sound is, relative to the way the listener faces: -1 left .. 1 right, scaled down close up (right at your head it is
// centred) and by kMaxPan.
void ApplyPanLocked(State& s, Voice& v) {
    if (!v.spatial) return;
    const float rx = v.pos[0] - s.listener[0], ry = v.pos[1] - s.listener[1], rz = v.pos[2] - s.listener[2];
    const float dist = std::sqrt(rx * rx + ry * ry + rz * rz);
    float pan = 0.0f;
    if (dist > 1e-3f) {
        const float* f = s.listenerForward; const float* u = s.listenerUp;
        const float right[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };   // forward x up
        const float rl = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (rl > 1e-4f) pan = (rx * right[0] + ry * right[1] + rz * right[2]) / (dist * rl);
        pan *= std::min(1.0f, dist);                       // inside a metre it drifts to the centre
    }
    const float p = std::fabs(std::clamp(pan, -1.0f, 1.0f)) * kMaxPan;
    ma_sound_set_pan(&v.sound, std::clamp(pan, -1.0f, 1.0f) * kMaxPan);
    // The balance leaves the near ear alone and lowers the far one, which would make a sound at the side quieter overall than one
    // ahead; this keeps the total power the same (ahead: 0.71 in each ear, the usual equal-power centre).
    const float gain = 1.0f / std::sqrt(1.0f + (1.0f - p) * (1.0f - p));
    if (std::fabs(gain - v.panGain) > 0.002f) { v.panGain = gain; ApplyVolume(v); }
}

Voice* FindLocked(State& s, Handle h) {
    for (auto& v : s.voices) if (v->id == h) return v.get();
    return nullptr;
}

Handle StartLocked(State& s, const std::shared_ptr<Clip>& clip, const std::string& path, const PlayParams& p, bool spatial, float x, float y, float z) {
    if (!s.open || !clip) return 0;
    ReapLocked(s);
    if (s.voices.size() >= kMaxVoices) s.voices.erase(s.voices.begin());   // the oldest gives way

    auto v = std::make_unique<Voice>();
    v->clip = clip;
    v->path = path;
    v->spatial = spatial;
    ma_audio_buffer_config bc = ma_audio_buffer_config_init(ma_format_f32, clip->channels, clip->frames, clip->pcm.data(), nullptr);
    bc.sampleRate = clip->sampleRate;
    if (ma_audio_buffer_init(&bc, &v->buffer) != MA_SUCCESS) return 0;
    v->bufferInit = true;
    // Sound rays: sound -> low-pass (muffle) -> delay (echo) -> endpoint, built before the sound starts. Flat sounds have none.
    ma_node* firstNode = nullptr;   // what the sound feeds
    if (spatial) {
        const ma_uint32 channels = ma_engine_get_channels(&s.engine), rate = ma_engine_get_sample_rate(&s.engine);
        ma_node_graph* graph = ma_engine_get_node_graph(&s.engine);
        ma_node* endpoint = ma_engine_get_endpoint(&s.engine);
        int echoVoices = 0;
        for (const auto& other : s.voices) if (other->delayInit) ++echoVoices;
        if (p.echoDelay > 0.0f && echoVoices < kMaxEchoVoices) {
            v->delayFrames = (ma_uint32)(std::clamp(p.echoDelay, 0.02f, 1.0f) * (float)rate);
            ma_delay_node_config dc = ma_delay_node_config_init(channels, rate, v->delayFrames, 0.0f);
            if (ma_delay_node_init(graph, &dc, nullptr, &v->delay) == MA_SUCCESS) {
                v->delayInit = true;
                v->echoWet = std::clamp(p.echoWet, 0.0f, 1.0f);
                v->echoFeedback = std::clamp(p.echoFeedback, 0.0f, 0.9f);
                ma_delay_node_set_dry(&v->delay, 1.0f);
                ma_delay_node_set_wet(&v->delay, v->echoWet);   // the rays keep it up to date through SetEcho
                ma_delay_node_set_decay(&v->delay, v->echoFeedback);
                ma_node_attach_output_bus(&v->delay, 0, endpoint, 0);
                firstNode = &v->delay;
            }
        }
        if (p.muffle) {
            v->cutoff = std::clamp(p.muffleCutoff, 200.0f, kMuffleCutoffOpen);
            v->muffleGain = std::clamp(p.muffleGain, 0.0f, 1.0f);
            ma_lpf_node_config lc = ma_lpf_node_config_init(channels, rate, v->cutoff, 2);
            if (ma_lpf_node_init(graph, &lc, nullptr, &v->lpf) == MA_SUCCESS) {
                v->lpfInit = true;
                ma_node_attach_output_bus(&v->lpf, 0, firstNode ? firstNode : endpoint, 0);
                firstNode = &v->lpf;
            }
        }
    }
    ma_sound_config sc = ma_sound_config_init_2(&s.engine);
    sc.pDataSource = &v->buffer;
    sc.flags = spatial ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (firstNode) sc.pInitialAttachment = firstNode;
    if (ma_sound_init_ex(&s.engine, &sc, &v->sound) != MA_SUCCESS) return 0;
    v->soundInit = true;

    v->volume = std::max(0.0f, p.volume);
    ApplyVolume(*v);
    ma_sound_set_pitch(&v->sound, std::clamp(p.pitch, 0.05f, 8.0f));
    if (!spatial) ma_sound_set_pan(&v->sound, std::clamp(p.pan, -1.0f, 1.0f));
    ma_sound_set_looping(&v->sound, p.loop ? MA_TRUE : MA_FALSE);
    if (spatial) {
        v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
        ma_sound_set_position(&v->sound, x, y, z);
        ma_sound_set_directional_attenuation_factor(&v->sound, 0.0f);   // our own, gentler left/right balance instead (kMaxPan)
        ApplyPanLocked(s, *v);
        ma_sound_set_min_distance(&v->sound, std::max(0.01f, p.minDistance));
        if (p.maxDistance > 0.0f) {
            ma_sound_set_attenuation_model(&v->sound, ma_attenuation_model_linear);
            ma_sound_set_max_distance(&v->sound, std::max(p.minDistance + 0.01f, p.maxDistance));
        }
    }
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
    if (s.failed) {
        if (std::chrono::steady_clock::now() - s.failedAt < std::chrono::seconds(kInitRetrySeconds)) return false;
        s.failed = false;   // try again: the device may be back
    }
    ma_engine_config cfg = ma_engine_config_init();
    if (noDevice) { cfg.noDevice = MA_TRUE; cfg.channels = 2; cfg.sampleRate = 48000; }
    const ma_result initResult = ma_engine_init(&cfg, &s.engine);
    if (initResult != MA_SUCCESS) {
        s.failed = true;
        s.failedAt = std::chrono::steady_clock::now();
        if (!s.failLogged) Log(std::string("[audio] no sound device, sounds will not play (") + ma_result_description(initResult) + ")");
        s.failLogged = true;
        return false;
    }
    s.open = true;
    s.failLogged = false;
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
    s.failLogged = false;
}

bool Available() { State& s = S(); std::lock_guard<std::mutex> lock(s.mutex); return s.open; }

Handle Play(const std::string& path, const PlayParams& params) {
    if (!Init()) return 0;
    State& s = S();
    auto clip = LoadClip(s, path);
    std::lock_guard<std::mutex> lock(s.mutex);
    return StartLocked(s, clip, path, params, false, 0, 0, 0);
}

Handle PlayAt(const std::string& path, float x, float y, float z, const PlayParams& params) {
    if (!Init()) return 0;
    State& s = S();
    auto clip = LoadClip(s, path);
    std::lock_guard<std::mutex> lock(s.mutex);
    return StartLocked(s, clip, path, params, true, x, y, z);
}

void Stop(Handle h) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(),
                   [h](const std::unique_ptr<Voice>& v) { return v->id == h; }), s.voices.end());
}

void StopAll() { State& s = S(); std::lock_guard<std::mutex> lock(s.mutex); s.voices.clear(); }

void StopByPath(const std::string& path) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.voices.erase(std::remove_if(s.voices.begin(), s.voices.end(),
                   [&](const std::unique_ptr<Voice>& v) { return v->path == path; }), s.voices.end());
}

bool IsPlaying(Handle h) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Voice* v = FindLocked(s, h);
    return v && ma_sound_at_end(&v->sound) == MA_FALSE;
}

void SetPosition(Handle h, float x, float y, float z) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Voice* v = FindLocked(s, h); v && v->spatial) {
        v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
        ma_sound_set_position(&v->sound, x, y, z);
        ApplyPanLocked(s, *v);
    }
}

void SetVolume(Handle h, float volume) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (Voice* v = FindLocked(s, h)) { v->volume = std::max(0.0f, volume); ApplyVolume(*v); }
}

void SetMuffle(Handle h, float cutoffHz, float gain) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Voice* v = FindLocked(s, h);
    if (!v) return;
    gain = std::clamp(gain, 0.0f, 1.0f);
    if (std::fabs(gain - v->muffleGain) > 0.01f) {
        v->muffleGain = gain;
        ApplyVolume(*v);
    }
    if (!v->lpfInit) return;
    cutoffHz = std::clamp(cutoffHz, 200.0f, kMuffleCutoffOpen);
    if (std::fabs(cutoffHz - v->cutoff) < v->cutoff * 0.05f) return;   // reinit only when it matters
    v->cutoff = cutoffHz;
    ma_lpf_config lc = ma_lpf_config_init(ma_format_f32, ma_engine_get_channels(&s.engine), ma_engine_get_sample_rate(&s.engine), cutoffHz, 2);
    ma_lpf_node_reinit(&lc, &v->lpf);
}

void SetEcho(Handle h, float wet, float feedback) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Voice* v = FindLocked(s, h);
    if (!v || !v->delayInit) return;
    wet = std::clamp(wet, 0.0f, 1.0f);
    feedback = std::clamp(feedback, 0.0f, 0.9f);
    if (std::fabs(wet - v->echoWet) > 0.005f) { v->echoWet = wet; ma_delay_node_set_wet(&v->delay, wet); }
    if (std::fabs(feedback - v->echoFeedback) > 0.005f) { v->echoFeedback = feedback; ma_delay_node_set_decay(&v->delay, feedback); }
}

bool HasEcho(Handle h) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    Voice* v = FindLocked(s, h);
    return v && v->delayInit;
}

void GetListener(float& x, float& y, float& z) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    x = s.listener[0]; y = s.listener[1]; z = s.listener[2];
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
    s.listener[0] = px; s.listener[1] = py; s.listener[2] = pz;
    s.listenerForward[0] = fx; s.listenerForward[1] = fy; s.listenerForward[2] = fz;
    s.listenerUp[0] = ux; s.listenerUp[1] = uy; s.listenerUp[2] = uz;
    if (!s.open) return;
    ma_engine_listener_set_position(&s.engine, 0, px, py, pz);
    ma_engine_listener_set_direction(&s.engine, 0, fx, fy, fz);
    ma_engine_listener_set_world_up(&s.engine, 0, ux, uy, uz);
}

void Update() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.open) return;
    ReapLocked(s);
    for (auto& v : s.voices) ApplyPanLocked(s, *v);   // you moved or turned
}

bool Preload(const std::string& path) {
    return LoadClip(S(), path) != nullptr;
}

bool GetClipInfo(const std::string& path, ClipInfo& out) {
    auto clip = LoadClip(S(), path);
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
