#pragma once
// Sound playback. Clips (WAV, MP3, FLAC, OGG Vorbis) are decoded once into memory and mixed by miniaudio
// (include/Engine/Backend/audio.h). Files come from the project folder or, in an exported game, from the
// archive (Vfs.hpp). The audio device is opened on the first Play, so a run that never plays a sound never
// touches the sound system; if there is no device (a CI box, a headless server) every call is a quiet no-op.
#include <cstdint>
#include <functional>
#include <string>

namespace audio {

using Handle = uint32_t;               // 0 = no sound (nothing played)

struct PlayParams {
    float volume = 1.0f;               // 0..; multiplied by the master volume
    float pitch  = 1.0f;               // 1 = as recorded; also changes the speed
    float pan    = 0.0f;               // -1 left .. 1 right (ignored for 3D sounds)
    bool  loop   = false;
};

// Where messages go ("[audio] ..."): no sound device, a file that can't be found or decoded. The default writes to stderr;
// the engine routes them to the editor's Output tab.
void SetLogHandler(std::function<void(const std::string&)> handler);

// Opens the sound system. Called by the first Play; call it earlier only to pay the start-up cost up front.
// `noDevice` mixes without a sound card: nothing is audible, and Pump() drives the mixer (tests).
bool Init(bool noDevice = false);
void Shutdown();                       // stops everything, closes the device, frees the clip cache
bool Available();                      // is the sound system open?

Handle Play(const std::string& path, const PlayParams& params = {});
// A sound located in the world: louder and panned by where it is relative to the listener.
Handle PlayAt(const std::string& path, float x, float y, float z, const PlayParams& params = {});

void   Stop(Handle h);
void   StopAll();
bool   IsPlaying(Handle h);
void   SetPosition(Handle h, float x, float y, float z);   // moves a 3D sound
void   SetVolume(Handle h, float volume);

void   SetMasterVolume(float volume);  // 0..1
float  MasterVolume();
void   SetListener(float px, float py, float pz, float fx, float fy, float fz, float ux, float uy, float uz);

// Per frame: frees sounds that finished.
void   Update();

// Decodes a clip ahead of time so its first Play doesn't hitch. False if the file is missing or unreadable.
bool   Preload(const std::string& path);
struct ClipInfo { uint64_t frames = 0; uint32_t channels = 0; uint32_t sampleRate = 0; };
bool   GetClipInfo(const std::string& path, ClipInfo& out);
void   ClearCache();

int    ActiveVoices();
// Mixes `frames` frames and returns how many were produced. Only for an engine opened with noDevice.
uint32_t Pump(uint32_t frames, float* out = nullptr);

} // namespace audio
