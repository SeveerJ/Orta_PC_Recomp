/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>
#include <mmreg.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_MAX_CHANNELS  6
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][XA2_MAX_CHANNELS];
static int                     g_xa2_channels = 2;
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;

int xa2_init(void)
{
    return xa2_init_channels(2) != 0;
}

int xa2_init_channels(int channels)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEXTENSIBLE wfx;
    XAUDIO2_VOICE_DETAILS md;

    if (g_xa2_initialized) return g_xa2_channels;
    if (channels != 6) channels = 2;
    memset(&wfx, 0, sizeof wfx);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        channels == 6 ? XAUDIO2_DEFAULT_CHANNELS : 2, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    /* Extensible format, so a six-channel voice says which speakers its
     * channels are: FL FR C LFE BL BR, the order the Xbox speaker bins use. */
    {
        static const GUID pcm = { 0x00000001, 0x0000, 0x0010,
                                  { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        wfx.Format.nChannels       = (WORD)channels;
        wfx.Format.nSamplesPerSec  = XA2_SAMPLE_RATE;
        wfx.Format.wBitsPerSample  = 16;
        wfx.Format.nBlockAlign     = (WORD)(channels * 2);
        wfx.Format.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.Format.nBlockAlign;
        wfx.Format.cbSize          = sizeof wfx - sizeof wfx.Format;
        wfx.Samples.wValidBitsPerSample = 16;
        wfx.dwChannelMask = channels == 6 ? 0x3Fu    /* SPEAKER_5POINT1 */
                                          : 0x03u;   /* SPEAKER_STEREO  */
        wfx.SubFormat = pcm;
    }

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    g_xa2_channels = channels;

    memset(&md, 0, sizeof md);
    g_xa2_master->lpVtbl->GetVoiceDetails(g_xa2_master, &md);
    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz, %d-channel 16-bit into a %u-channel device, "
            "%d x %d-sample buffers)\n", XA2_SAMPLE_RATE, channels, (unsigned)md.InputChannels,
            XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return md.InputChannels ? (int)md.InputChannels : channels;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) return 0;

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * g_xa2_channels * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * g_xa2_channels * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- POSIX stubs (no audio output yet) */

int  xa2_init(void)                                   { return 0; }
int  xa2_init_channels(int c)                          { (void)c; return 0; }
void xa2_shutdown(void)                               {}
int  xa2_is_active(void)                              { return 0; }
int  xa2_submit_samples(const int16_t *s, int n)      { (void)s; (void)n; return 0; }
int  xa2_get_buffer_size(void)                        { return 0; }

#endif /* _WIN32 */
