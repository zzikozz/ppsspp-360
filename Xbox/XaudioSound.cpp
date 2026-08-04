#include <xtl.h>
#include <PPCIntrinsics.h>
#include <xaudio2.h>
#include "Common/FileUtil.h"
#include "Common/LogManager.h"
#include "Core/PSPMixer.h"
#include "Core/CPU.h"
#include "Core/Config.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/Host.h"
#include "Core/SaveState.h"
#include "Common/MemArena.h"

#include "XaudioSound.h"

// Double-buffering: 2 buffers of 1024 stereo samples each.
// 1024 samples at 44100Hz = ~23ms per buffer. Two buffers give
// ~46ms of audio in flight, enough to survive callback jitter.
#define SAMPLES_PER_BUFFER  1024
#define NUM_BUFFERS         2
#define BYTES_PER_SAMPLE    4   // stereo 16-bit = 2 channels * 2 bytes

static short xaudio_buffers[NUM_BUFFERS][SAMPLES_PER_BUFFER * 2];
static int nextBuffer = 0;

static WAVEFORMATEX wfx;
static IXAudio2 *lpXAudio2 = NULL;
static IXAudio2MasteringVoice *lpMasterVoice = NULL;
static IXAudio2SourceVoice *lpSourceVoice = NULL;

static PMixer *g_mixer = 0;
static CRITICAL_SECTION audioCriticalSection;
static bool audioCritSecInit = false;
static volatile bool shuttingDown = false;

static int NativeMix(short *audio, int num_samples);

class XAudioCallback : public IXAudio2VoiceCallback
{
public:
    void OnBufferEnd(void *pBufferContext) {
        XAudioUpdate();
    }

    XAudioCallback() {}
    ~XAudioCallback() {}
    void OnVoiceProcessingPassEnd() {}
    void OnVoiceProcessingPassStart(UINT32 SamplesRequired) {}
    void OnStreamEnd() {}
    void OnBufferStart(void *pBufferContext) {}
    void OnLoopEnd(void *pBufferContext) {}
    void OnVoiceError(void *pBufferContext, HRESULT Error) {}
};

static XAudioCallback voice;

int NativeMix(short *audio, int num_samples) {
    if (g_mixer) {
        num_samples = g_mixer->Mix(audio, num_samples);
    } else {
        memset(audio, 0, num_samples * 2 * sizeof(short));
    }

    // XAudio2 on Xbox 360 expects native big-endian PCM.
    // The PSP audio pipeline produces little-endian s16 (bswap16 applied
    // before writing to PSP memory), so we must byte-swap before submitting.
    unsigned short * ptr = (unsigned short*)audio;
    for (int i = 0; i < num_samples; i++) {
        ptr[i] = _byteswap_ushort(ptr[i]);
    }

    return num_samples;
}

void XAudioInit(PMixer *mixer) {
    g_mixer = mixer;
    nextBuffer = 0;
    shuttingDown = false;

    InitializeCriticalSection(&audioCriticalSection);
    audioCritSecInit = true;

    if (FAILED(XAudio2Create(&lpXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        DebugBreak();
        return;
    }

    if (FAILED(lpXAudio2->CreateMasteringVoice(&lpMasterVoice, 2, 48000, 0, 0, NULL))) {
        XAudioShutdown();
        return;
    }

    memset(&wfx, 0, sizeof(WAVEFORMATEX));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nSamplesPerSec = 44100;
    wfx.nChannels = 2;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
    wfx.cbSize = 0;

    if (FAILED(lpXAudio2->CreateSourceVoice(&lpSourceVoice, &wfx,
                 0, XAUDIO2_DEFAULT_FREQ_RATIO, &voice, NULL, NULL))) {
        XAudioShutdown();
        return;
    }

    // Pre-fill and submit all buffers so there's always audio in the pipeline.
    // This eliminates the startup gap and gives ~46ms of buffered audio.
    for (int i = 0; i < NUM_BUFFERS; i++) {
        EnterCriticalSection(&audioCriticalSection);
        int numSamples = NativeMix(xaudio_buffers[i], SAMPLES_PER_BUFFER);
        if (numSamples <= 0) {
            memset(xaudio_buffers[i], 0, SAMPLES_PER_BUFFER * BYTES_PER_SAMPLE);
            numSamples = SAMPLES_PER_BUFFER;
        }
        LeaveCriticalSection(&audioCriticalSection);

        XAUDIO2_BUFFER buf = {0};
        buf.AudioBytes = numSamples * BYTES_PER_SAMPLE;
        buf.pAudioData = (BYTE*)xaudio_buffers[i];
        lpSourceVoice->SubmitSourceBuffer(&buf);
    }

    lpSourceVoice->Start(0, 0);
}

void XAudioUpdate() {
    EnterCriticalSection(&audioCriticalSection);

    if (shuttingDown || !lpSourceVoice) {
        LeaveCriticalSection(&audioCriticalSection);
        return;
    }

    int idx = nextBuffer;
    nextBuffer = (nextBuffer + 1) % NUM_BUFFERS;

    int numSamples = NativeMix(xaudio_buffers[idx], SAMPLES_PER_BUFFER);
    if (numSamples <= 0) {
        memset(xaudio_buffers[idx], 0, SAMPLES_PER_BUFFER * BYTES_PER_SAMPLE);
        numSamples = SAMPLES_PER_BUFFER;
    }

    XAUDIO2_BUFFER buf = {0};
    buf.AudioBytes = numSamples * BYTES_PER_SAMPLE;
    buf.pAudioData = (BYTE*)xaudio_buffers[idx];
    lpSourceVoice->SubmitSourceBuffer(&buf);

    LeaveCriticalSection(&audioCriticalSection);
}

void XAudioShutdown() {
    // Signal shutdown and wait for any in-flight callback to finish.
    // The callback checks shuttingDown under the CS and returns early.
    EnterCriticalSection(&audioCriticalSection);
    shuttingDown = true;
    LeaveCriticalSection(&audioCriticalSection);
    // Any callback that was in progress has now seen the flag and released CS.

    if (lpSourceVoice) {
        lpSourceVoice->Stop(0);
        lpSourceVoice->FlushSourceBuffers();
        // No more callbacks will fire after FlushSourceBuffers returns.
    }

    EnterCriticalSection(&audioCriticalSection);
    if (lpSourceVoice) {
        lpSourceVoice->DestroyVoice();
        lpSourceVoice = NULL;
    }
    if (lpMasterVoice) {
        lpMasterVoice->DestroyVoice();
        lpMasterVoice = NULL;
    }
    LeaveCriticalSection(&audioCriticalSection);

    if (lpXAudio2) {
        lpXAudio2->Release();
        lpXAudio2 = NULL;
    }

    if (audioCritSecInit) {
        DeleteCriticalSection(&audioCriticalSection);
        audioCritSecInit = false;
    }

    g_mixer = NULL;
}
