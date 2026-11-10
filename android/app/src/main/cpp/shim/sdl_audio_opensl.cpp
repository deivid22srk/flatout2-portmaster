// shim/sdl_audio_opensl.cpp — SDL2 audio over OpenSL ES.
//
// The guest registers a callback (glibc code, needs the guest TCB). OpenSL
// callbacks run on bionic threads, so the design is:
//
//   guest pump thread (guest TCB)          OpenSL callback (bionic thread)
//   --------------------------------       -------------------------------
//   wait sem -> SDL_AudioCallback(userdata, staging, bytes)
//           -> push into SPSC ring    <-   pull from ring -> Enqueue
//
// The device always runs S16 stereo at the desired rate; F32 output (the
// game's mixer) is converted in the pump thread.
#include "sdl_state.h"
#include "shim_common.h"
#include "../core/guest_thread.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <android/log.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

using namespace fo2;
using fo2::BionicGuard;
using fo2::logi;

namespace {

SLObjectItf g_engine = nullptr;
SLObjectItf g_mix = nullptr;

struct Pump {
    // guest side
    SDL_AudioCallback callback;
    void* userdata;
    int bytes_per_chunk;

    // SPSC ring (pump writes, OpenSL reads)
    uint8_t* ring = nullptr;
    size_t ring_size = 0;
    size_t rd = 0, wr = 0;   // bytes

    // OpenSL side
    SLObjectItf player = nullptr;
    SLPlayItf play = nullptr;
    SLBufferQueueItf queue = nullptr;
    uint8_t* sl_buf = nullptr;

    std::atomic<bool> stop{false};

    size_t used() const {
        return (wr >= rd) ? (wr - rd) : (ring_size - rd + wr);
    }
    size_t free_space() const { return ring_size - 1 - used(); }
};

bool ring_write(Pump* p, const uint8_t* data, size_t len) {
    if (p->free_space() < len) return false;
    size_t tail = p->wr % p->ring_size;
    size_t first = (len < p->ring_size - tail) ? len : (p->ring_size - tail);
    memcpy(p->ring + tail, data, first);
    memcpy(p->ring, data + first, len - first);
    p->wr += len;
    return true;
}

size_t ring_read(Pump* p, uint8_t* out, size_t len) {
    size_t avail = p->used();
    if (avail > len) avail = len;
    size_t head = p->rd % p->ring_size;
    size_t first = (avail < p->ring_size - head) ? avail : (p->ring_size - head);
    memcpy(out, p->ring + head, first);
    memcpy(out + first, p->ring, avail - first);
    p->rd += avail;
    return avail;
}

void opensl_callback(SLBufferQueueItf q, void* ctx) {
    Pump* p = (Pump*)ctx;
    size_t want = (size_t)p->bytes_per_chunk;
    size_t got = ring_read(p, p->sl_buf, want);
    if (got < want) memset(p->sl_buf + got, 0, want - got);  // underrun -> silence
    (*q)->Enqueue(q, p->sl_buf, (SLuint32)want);
}

void* pump_loop(void* arg) {
    Pump* p = (Pump*)arg;

    // Pump runs as a GUEST thread (created via run_guest_fn): the audio callback
    // is guest code and needs the guest TCB. Bionic calls below use BionicGuard.
    auto* staging = (uint8_t*)malloc((size_t)p->bytes_per_chunk);
    auto* converted = (uint8_t*)malloc((size_t)p->bytes_per_chunk);
    bool f32 = g_audio.desired.format == AUDIO_F32SYS;

    while (!p->stop.load()) {
        if (!g_audio.running.load() || p->callback == nullptr) {
            usleep(4000);  // paused: OpenSL drains the ring into silence
            continue;
        }
        p->callback(p->userdata, staging, p->bytes_per_chunk);

        const uint8_t* out = staging;
        if (f32) {
            const float* src = (const float*)staging;
            int16_t* dst = (int16_t*)converted;
            size_t n = (size_t)p->bytes_per_chunk / sizeof(float);
            for (size_t i = 0; i < n; ++i) {
                float s = src[i];
                if (s > 1.f) s = 1.f;
                if (s < -1.f) s = -1.f;
                dst[i] = (int16_t)(s * 32767.f);
            }
            out = converted;
        }
        {
            BionicGuard g;
            // Wait until the ring has room (OpenSL keeps draining).
            while (!p->stop.load() && p->free_space() < (size_t)p->bytes_per_chunk) {
                usleep(1000);
            }
            if (!p->stop.load()) ring_write(p, out, (size_t)p->bytes_per_chunk);
        }
    }
    free(staging);
    free(converted);
    return nullptr;
}

bool ensure_engine() {
    if (g_engine != nullptr) return true;
    {
        BionicGuard g;
        if (slCreateEngine(&g_engine, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS)
            return false;
        if ((*g_engine)->Realize(g_engine, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
            return false;
        SLEngineItf eng;
        (*g_engine)->GetInterface(g_engine, SL_IID_ENGINE, &eng);
        if ((*eng)->CreateOutputMix(eng, &g_mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS)
            return false;
        if ((*g_mix)->Realize(g_mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS)
            return false;
    }
    return true;
}

} // namespace

extern "C" SDL_AudioDeviceID sdl_SDL_OpenAudioDevice(const char*, int iscapture,
                                                     const SDL_AudioSpec* desired,
                                                     SDL_AudioSpec* obtained,
                                                     int allowed_changes) {
    if (iscapture || desired == nullptr || obtained == nullptr) return 0;
    if (g_audio.open.load()) return 0;  // single device, like the port's usage

    if (!ensure_engine()) return 0;

    auto* pump = new Pump();
    pump->callback = desired->callback;
    pump->userdata = desired->userdata;
    pump->bytes_per_chunk = (desired->samples > 0 ? desired->samples : 1024) *
                            2 /* stereo */ * 2 /* S16 */;

    // Ring of 8 chunks.
    pump->ring_size = pump->bytes_per_chunk * 8;
    pump->ring = (uint8_t*)malloc(pump->ring_size);
    pump->sl_buf = (uint8_t*)malloc((size_t)pump->bytes_per_chunk);

    // SDL format bookkeeping: the guest always gets what it asked for.
    g_audio.desired = *desired;
    g_audio.obtained = *desired;
    if (g_audio.obtained.samples <= 0) g_audio.obtained.samples = 1024;
    g_audio.obtained.size = g_audio.obtained.samples * g_audio.obtained.channels * 2;
    g_audio.callback = desired->callback;
    g_audio.userdata = desired->userdata;

    // (allowed_changes accepted: we serve the exact desired spec either way.)

    {
        BionicGuard g;
        SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, g_mix};
        SLDataSink audio_sink = {&loc_outmix, nullptr};
        SLDataLocator_BufferQueue loc_bufq = {SL_DATALOCATOR_BUFFERQUEUE, 4};
        SLDataFormat_PCM pcm = {
            SL_DATAFORMAT_PCM, 2, (SLuint32)(desired->freq > 0 ? desired->freq : 48000),
            SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
            SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
            SL_BYTEORDER_LITTLEENDIAN };
        SLDataSource src = {&loc_bufq, &pcm};

        SLEngineItf eng;
        (*g_engine)->GetInterface(g_engine, SL_IID_ENGINE, &eng);
        if ((*eng)->CreateAudioPlayer(eng, &pump->player, &src, &audio_sink, 0, nullptr, nullptr)
                != SL_RESULT_SUCCESS) {
            logi("OpenSL CreateAudioPlayer failed");
            delete pump;
            return 0;
        }
        (*pump->player)->Realize(pump->player, SL_BOOLEAN_FALSE);
        (*pump->player)->GetInterface(pump->player, SL_IID_PLAY, &pump->play);
        (*pump->player)->GetInterface(pump->player, SL_IID_BUFFERQUEUE, &pump->queue);
        (*pump->queue)->RegisterCallback(pump->queue, opensl_callback, pump);

        // Pre-fill with silence and start.
        memset(pump->sl_buf, 0, (size_t)pump->bytes_per_chunk);
        (*pump->queue)->Enqueue(pump->queue, pump->sl_buf, (SLuint32)pump->bytes_per_chunk);
        (*pump->queue)->Enqueue(pump->queue, pump->sl_buf, (SLuint32)pump->bytes_per_chunk);
        (*pump->play)->SetPlayState(pump->play, SL_PLAYSTATE_PLAYING);
    }

    g_audio.open.store(true);
    g_audio.running.store(true);

    // Pump thread runs as a guest thread (audio callback is guest code).
    if (!fo2::run_guest_fn([](void* arg) -> void* {
            return pump_loop(arg);
        }, pump, 1024 * 1024)) {
        logi("audio pump thread failed");
        g_audio.open.store(false);
        delete pump;
        return 0;
    }

    *obtained = g_audio.obtained;
    logi("SDL_OpenAudioDevice ok: %d Hz %d ch", obtained->freq, obtained->channels);
    return 1;
}

extern "C" void sdl_SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause_on) {
    g_audio.running.store(pause_on == 0);
}

extern "C" void sdl_SDL_CloseAudioDevice(SDL_AudioDeviceID) {
    g_audio.open.store(false);
    g_audio.running.store(false);
    // Player/keep engine for potential restart; the process is torn down on exit.
}

extern "C" const char* sdl_SDL_GetCurrentAudioDriver(void) {
    return "opensl";
}
