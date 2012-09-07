/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2012 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_config.h"

#if SDL_AUDIO_DRIVER_ANDROID

/* Output audio to Android */

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "SDL_androidaudio.h"
#include "../SDL_audiomem.h"

#include "../../core/android/SDL_android.h"

#include <android/log.h>

#define LOG_NDEBUG 0
#define LOG_TAG "SDL_AndroidAUD"
#include <utils/Log.h>


static void * audioDevice;

#if ENABLE_NATIVE_AUDIO
#include <dlfcn.h>

// _ZN7android11AudioSystem19getOutputFrameCountEPii
typedef int (*AudioSystem_getOutputFrameCount)(int *, int);
// _ZN7android11AudioSystem16getOutputLatencyEPji
typedef int (*AudioSystem_getOutputLatency)(unsigned int *, int);
// _ZN7android11AudioSystem21getOutputSamplingRateEPii
typedef int (*AudioSystem_getOutputSamplingRate)(int *, int);

// _ZN7android10AudioTrack16getMinFrameCountEPiij
typedef int (*AudioTrack_getMinFrameCount)(int *, int, unsigned int);

// _ZN7android10AudioTrackC1EijiiijPFviPvS1_ES1_ii
typedef void (*AudioTrack_ctor)(void *, int, unsigned int, int, int, int, unsigned int, void (*)(int, void *, void *), void *, int, int);
// _ZN7android10AudioTrackC1EijiiijPFviPvS1_ES1_i
typedef void (*AudioTrack_ctor_legacy)(void *, int, unsigned int, int, int, int, unsigned int, void (*)(int, void *, void *), void *, int);
// _ZN7android10AudioTrackD1Ev
typedef void (*AudioTrack_dtor)(void *);
// _ZNK7android10AudioTrack9initCheckEv
typedef int (*AudioTrack_initCheck)(void *);
// _ZN7android10AudioTrack5startEv
typedef int (*AudioTrack_start)(void *);
// _ZN7android10AudioTrack4stopEv
typedef int (*AudioTrack_stop)(void *);
// _ZN7android10AudioTrack5writeEPKvj
typedef int (*AudioTrack_write)(void *, void  const*, unsigned int);
// _ZN7android10AudioTrack5flushEv
typedef int (*AudioTrack_flush)(void *);

static AudioSystem_getOutputFrameCount as_getOutputFrameCount = NULL;
static AudioSystem_getOutputLatency as_getOutputLatency = NULL;
static AudioSystem_getOutputSamplingRate as_getOutputSamplingRate = NULL;
static AudioTrack_getMinFrameCount at_getMinFrameCount = NULL;
static AudioTrack_ctor at_ctor = NULL;
static AudioTrack_ctor_legacy at_ctor_legacy = NULL;
static AudioTrack_dtor at_dtor = NULL;
static AudioTrack_initCheck at_initCheck = NULL;
static AudioTrack_start at_start = NULL;
static AudioTrack_stop at_stop = NULL;
static AudioTrack_write at_write = NULL;
static AudioTrack_flush at_flush = NULL;

void *g_plibrary = NULL;

static void *InitLibrary() {
    void *p_library;

    LOGV("InitLibrary...");
    p_library = dlopen("libmedia.so", RTLD_NOW);
    if (!p_library) {
        LOGV("InitLibrary dlopen error...");
        return NULL;
    }
    LOGV("InitLibrary dlopen ok...");
    as_getOutputFrameCount = (AudioSystem_getOutputFrameCount)(dlsym(p_library, "_ZN7android11AudioSystem19getOutputFrameCountEPii"));
    as_getOutputLatency = (AudioSystem_getOutputLatency)(dlsym(p_library, "_ZN7android11AudioSystem16getOutputLatencyEPji"));
    as_getOutputSamplingRate = (AudioSystem_getOutputSamplingRate)(dlsym(p_library, "_ZN7android11AudioSystem21getOutputSamplingRateEPii"));
    at_getMinFrameCount = (AudioTrack_getMinFrameCount)(dlsym(p_library, "_ZN7android10AudioTrack16getMinFrameCountEPiij"));
    at_ctor = (AudioTrack_ctor)(dlsym(p_library, "_ZN7android10AudioTrackC1EijiiijPFviPvS1_ES1_ii"));
    at_ctor_legacy = (AudioTrack_ctor_legacy)(dlsym(p_library, "_ZN7android10AudioTrackC1EijiiijPFviPvS1_ES1_i"));
    at_dtor = (AudioTrack_dtor)(dlsym(p_library, "_ZN7android10AudioTrackD1Ev"));
    at_initCheck = (AudioTrack_initCheck)(dlsym(p_library, "_ZNK7android10AudioTrack9initCheckEv"));
    at_start = (AudioTrack_start)(dlsym(p_library, "_ZN7android10AudioTrack5startEv"));
    at_stop = (AudioTrack_stop)(dlsym(p_library, "_ZN7android10AudioTrack4stopEv"));
    at_write = (AudioTrack_write)(dlsym(p_library, "_ZN7android10AudioTrack5writeEPKvj"));
    at_flush = (AudioTrack_flush)(dlsym(p_library, "_ZN7android10AudioTrack5flushEv"));
    // need the first 3 or the last 1
    if (!((as_getOutputFrameCount && as_getOutputLatency && as_getOutputSamplingRate) || at_getMinFrameCount)) {
        LOGV("InitLibrary check error 1...");
        dlclose(p_library);
        return NULL;
    }
    // need all in the list
    if (!((at_ctor || at_ctor_legacy) && at_dtor && at_initCheck && at_start && at_stop && at_write && at_flush)) {
        LOGV("InitLibrary check error 2...");
        dlclose(p_library);
        return NULL;
    }
    LOGV("InitLibrary InitLibrary ok...");
    return p_library;
}

#endif // ENABLE_NATIVE_AUDIO

static void
AndroidAUD_CloseDevice(_THIS)
{
    if (this->hidden != NULL) {
#if ENABLE_NATIVE_AUDIO
        LOGV("AndroidAUD_CloseDevice...");
        if (this->hidden->mixbuf != NULL) {
            SDL_FreeAudioMem(this->hidden->mixbuf);
            this->hidden->mixbuf = NULL;
        }
        if (this->hidden->AudioTrack != NULL) {
            at_stop(this->hidden->AudioTrack);
            at_flush(this->hidden->AudioTrack);
            at_dtor(this->hidden->AudioTrack);
            SDL_FreeAudioMem(this->hidden->AudioTrack);
            this->hidden->AudioTrack = NULL;
        }
        if (this->hidden->libmedia != NULL) {
            this->hidden->libmedia = NULL;
        }
#endif
    	SDL_free(this->hidden);
    	this->hidden = NULL;
    }

#if !ENABLE_NATIVE_AUDIO
    Android_JNI_CloseAudioDevice();
#endif

    if (audioDevice == this) {
    	audioDevice = NULL;
    }
}

static void
AndroidAUD_PlayDevice(_THIS)
{
#if !ENABLE_NATIVE_AUDIO
    Android_JNI_WriteAudioBuffer();
#else
    int length;
    const Uint8 *sample_buf = (const Uint8 *) this->hidden->mixbuf;
    const int frame_size = (((int) (this->spec.format & 0xFF)) / 8) *
                                this->spec.channels;
    int frames_left = this->spec.samples;

    while ( frames_left > 0 && this->enabled ) {
        /* !!! FIXME: This works, but needs more testing before going live */
        length = at_write(this->hidden->AudioTrack, (char*)sample_buf, frames_left * frame_size);
        sample_buf += length;
        frames_left -= length/frame_size;
    }
#endif
}

static Uint8 *
AndroidAUD_GetDeviceBuf(_THIS)
{
#if !ENABLE_NATIVE_AUDIO
    return Android_JNI_GetAudioBuffer();
#else
    return (this->hidden->mixbuf);
#endif
}


static int
AndroidAUD_OpenDevice(_THIS, const char *devname, int iscapture)
{
#if ENABLE_NATIVE_AUDIO
    LOGV("AndroidAUD_OpenDevice...");
    void *AudioTrack;
    int status;
    int type, freq, format, channel, samples, nb_samples;
    int afSampleRate, afFrameCount, afLatency, minBufCount, minFrameCount;
#endif
    SDL_AudioFormat test_format;
    int valid_datatype = 0;
    
    if (iscapture) {
    	//TODO: implement capture
    	SDL_SetError("Capture not supported on Android");
    	return 0;
    }

    if (audioDevice != NULL) {
    	SDL_SetError("Only one audio device at a time please!");
    	return 0;
    }

    audioDevice = this;

    this->hidden = SDL_malloc(sizeof(*(this->hidden)));
    if (!this->hidden) {
        SDL_OutOfMemory();
        return 0;
    }
    SDL_memset(this->hidden, 0, (sizeof *this->hidden));

    test_format = SDL_FirstAudioFormat(this->spec.format);
    while (test_format != 0) { // no "UNKNOWN" constant
        if ((test_format == AUDIO_U8) || (test_format == AUDIO_S16LSB)) {
            this->spec.format = test_format;
            break;
        }
        test_format = SDL_NextAudioFormat();
    }
    
    if (test_format == 0) {
    	// Didn't find a compatible format :(
        AndroidAUD_CloseDevice(this);
    	SDL_SetError("No compatible audio format!");
    	return 0;
    }

    if (this->spec.channels > 1) {
    	this->spec.channels = 2;
    } else {
    	this->spec.channels = 1;
    }

    // 4000 <= frequency <= 48000
    if (this->spec.freq < 8000) {
    	this->spec.freq = 8000;
    }
    if (this->spec.freq > 48000) {
    	this->spec.freq = 48000;
    }

#if !ENABLE_NATIVE_AUDIO
    // TODO: pass in/return a (Java) device ID, also whether we're opening for input or output
    this->spec.samples = Android_JNI_OpenAudioDevice(this->spec.freq, this->spec.format == AUDIO_U8 ? 0 : 1, this->spec.channels, this->spec.samples);
    SDL_CalculateAudioSpec(&this->spec);

    if (this->spec.samples == 0) {
    	// Init failed?
        AndroidAUD_CloseDevice(this);
    	SDL_SetError("Java-side initialization failed!");
    	return 0;
    }
#else
    this->hidden->libmedia = g_plibrary;

    // AudioSystem::MUSIC = 3
    type = 3;

    freq = this->spec.freq;

    // AudioSystem::PCM_16_BIT = 1
    // AudioSystem::PCM_8_BIT = 2
    format = this->spec.format == AUDIO_U8 ? 2 : 1;

    // AudioSystem::CHANNEL_OUT_STEREO = 12
    // AudioSystem::CHANNEL_OUT_MONO = 4
    channel = (this->spec.channels == 2) ? 12 : 4;

    // use the minium value
    if (!at_getMinFrameCount) {
        status = as_getOutputSamplingRate(&afSampleRate, type);
        status ^= as_getOutputFrameCount(&afFrameCount, type);
        status ^= as_getOutputLatency((uint32_t*)(&afLatency), type);
        if (status != 0) {
            AndroidAUD_CloseDevice(this);
            return 0;
        }
        minBufCount = afLatency / ((1000 * afFrameCount) / afSampleRate);
        if (minBufCount < 2)
            minBufCount = 2;
        minFrameCount = (afFrameCount * freq * minBufCount) / afSampleRate;
        nb_samples = minFrameCount;
    }
    else {
        status = at_getMinFrameCount(&nb_samples, type, freq);
        if (status != 0) {
            AndroidAUD_CloseDevice(this);
            return 0;
        }
    }
    nb_samples <<= 1;
    samples = nb_samples;

    this->spec.samples = samples;

    // sizeof(AudioTrack) == 0x58 (not sure) on 2.2.1, this should be enough
    AudioTrack = SDL_AllocAudioMem(256);
    if (!AudioTrack) {
        AndroidAUD_CloseDevice(this);
        SDL_OutOfMemory();
        return 0;
    }
    this->hidden->AudioTrack = AudioTrack;

    // higher than android 2.2
    if (at_ctor)
        at_ctor(AudioTrack, type, freq, format, channel, samples, 0, NULL, NULL, 0, 0);
    // higher than android 1.6
    else if (at_ctor_legacy)
        at_ctor_legacy(AudioTrack, type, freq, format, channel, samples, 0, NULL, NULL, 0);
    status = at_initCheck(AudioTrack);
    // android 1.6
    if (status != 0) {
        at_ctor_legacy(AudioTrack, type, freq, format, channel, samples, 0, NULL, NULL, 0);
        status = at_initCheck(AudioTrack);
    }
    if (status != 0) {
        AndroidAUD_CloseDevice(this);
        SDL_SetError("Cannot create AudioTrack!");
        return 0;
    }

    SDL_CalculateAudioSpec(&this->spec);

    if (this->spec.samples == 0) {
    	// Init failed?
        AndroidAUD_CloseDevice(this);
    	SDL_SetError("Audio initialization failed!");
    	return 0;
    }

    /* Allocate mixing buffer */
    this->hidden->mixlen = this->spec.size;
    this->hidden->mixbuf = (Uint8 *) SDL_AllocAudioMem(this->hidden->mixlen);
    if (this->hidden->mixbuf == NULL) {
        AndroidAUD_CloseDevice(this);
        SDL_OutOfMemory();
        return 0;
    }
    SDL_memset(this->hidden->mixbuf, this->spec.silence, this->spec.size);

    // start here?
    at_start(this->hidden->AudioTrack);

#endif // ENABLE_NATIVE_AUDIO

    return 1;
}

#if ENABLE_NATIVE_AUDIO
static int
LoadAndroidAUDLibrary(void) {
    LOGV("LoadAndroidAUDLibrary...");
    int retval = 0;
    if (g_plibrary == NULL) {
        g_plibrary = InitLibrary();
        if (g_plibrary == NULL) {
            SDL_SetError("Could not initialize libmedia.so!");
            retval = -1;
        }
    }
    return retval;
}

static void
UnloadAndroidAUDLibrary(void) {
    if (g_plibrary != NULL) {
        dlclose(g_plibrary);
    }
}

/* This function waits until it is possible to write a full sound buffer */
static void
AndroidAUD_WaitDevice(_THIS)
{
    /* We're in blocking mode, so there's nothing to do here */
}

static void
AndroidAUD_Deinitialize(void)
{
    LOGV("AndroidAUD_Deinitialize...");
    UnloadAndroidAUDLibrary();
}
#endif

static int
AndroidAUD_Init(SDL_AudioDriverImpl * impl)
{
#if ENABLE_NATIVE_AUDIO
    LOGV("AndroidAUD_Init...");
    if (LoadAndroidAUDLibrary() < 0) {
        return 0;
    }
    LOGV("AndroidAUD_Init LoadAndroidAUDLibrary ok");
#endif

    /* Set the function pointers */
    impl->OpenDevice = AndroidAUD_OpenDevice;
    impl->PlayDevice = AndroidAUD_PlayDevice;
    impl->GetDeviceBuf = AndroidAUD_GetDeviceBuf;
    impl->CloseDevice = AndroidAUD_CloseDevice;
#if ENABLE_NATIVE_AUDIO
    impl->WaitDevice = AndroidAUD_WaitDevice;
    impl->Deinitialize = AndroidAUD_Deinitialize;
#endif

#if !ENABLE_NATIVE_AUDIO
    /* and the capabilities */
    impl->ProvidesOwnCallbackThread = 1;
    impl->HasCaptureSupport = 0; //TODO
    impl->OnlyHasDefaultInputDevice = 1;
#endif
    impl->OnlyHasDefaultOutputDevice = 1;

    return 1;   /* this audio target is available. */
}

AudioBootStrap ANDROIDAUD_bootstrap = {
    "android", "SDL Android audio driver", AndroidAUD_Init, 0
};

/* Called by the Java code to start the audio processing on a thread */
void
Android_RunAudioThread()
{
	SDL_RunAudio(audioDevice);
}

#endif /* SDL_AUDIO_DRIVER_ANDROID */

/* vi: set ts=4 sw=4 expandtab: */
