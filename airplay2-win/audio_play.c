#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_play.h"

/* 简单音频队列每缓冲信息 */
typedef struct audio_buf_s {
    WAVEHDR hdr;
    uint8_t *buf;
    int bytes;
    int samples; /* samples per channel in this buffer */
    uint64_t pts_ms; /* start timestamp for this buffer */
} audio_buf_t;

static HWAVEOUT g_hWaveOut = NULL;
static WAVEFORMATEX g_wf = {0};
static CRITICAL_SECTION g_cs;
static volatile int g_inited = 0;

/* 统计已全部播放完成的样本数（每通道）*/
static uint64_t g_played_samples = 0;
/* 音频参数 */
static int g_sample_rate = 0;
static int g_channels = 0;
static int g_bytes_per_sample = 0;

/* waveOut 回调处理 WOM_DONE，释放缓冲并累加已播放样本 */
static void CALLBACK wave_out_proc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    (void)hwo; (void)dwInstance; (void)dwParam2;
    if (uMsg == WOM_DONE) {
        WAVEHDR* ph = (WAVEHDR*)dwParam1;
        if (ph && ph->dwUser) {
            audio_buf_t* ab = (audio_buf_t*)ph->dwUser;
            /* 累计样本数（每通道） */
            EnterCriticalSection(&g_cs);
            g_played_samples += (uint64_t)ab->samples;
            LeaveCriticalSection(&g_cs);

            /* 释放 */
            if (ab->buf) free(ab->buf);
            free(ab);
        }
    }
}

/* 把 pcm_data_struct 的 pts 统一转换为毫秒（ms）。
   启发式检测常见单位：
   - 若 sample_rate==0，直接返回原始 pts。
   - 若 pts > sample_rate*1000：很可能是样本计数（samples），按 samples->ms 转换。
   - 若 pts > 1e9：很可能为微秒，除以1000 转为 ms。
   - 若 pts > 1000：很可能已经是毫秒，直接返回。
   - 否则当作秒，乘以1000。
   如果你知道 pts 的精确单位（例如始终为字节偏移、或始终为毫秒），请替换为确定性换算以消除歧义。 */
uint64_t audio_pts_to_ms(const pcm_data_struct* a)
{
    if (!a) return 0;
    uint64_t pts = (uint64_t)a->pts;

    if (a->sample_rate == 0) {
        return pts;
    }

    if (pts == 0) {
        return 0;
    }

    /* 如果 pts 很大，可能表示样本数量（samples），samples->ms = samples * 1000 / sample_rate */
    if (pts > (uint64_t)a->sample_rate * 1000ULL) {
        return (pts * 1000ULL) / (uint64_t)a->sample_rate;
    }

    /* 如果 pts 很大（>1e9），可能是微秒 */
    if (pts > 1000000000ULL) {
        return pts / 1000ULL;
    }

    /* 如果 pts 在合理毫秒范围，直接视作毫秒 */
    if (pts > 1000ULL) {
        return pts;
    }

    /* 小值，可能为秒 */
    return pts * 1000ULL;
}

int audio_play_init(int sample_rate, int channels, int bits_per_sample)
{
    if (g_inited) return 0;
    InitializeCriticalSection(&g_cs);

    g_sample_rate = sample_rate;
    g_channels = channels;
    g_bytes_per_sample = bits_per_sample / 8;

    memset(&g_wf, 0, sizeof(g_wf));
    g_wf.wFormatTag = WAVE_FORMAT_PCM;
    g_wf.nChannels = (WORD)channels;
    g_wf.nSamplesPerSec = sample_rate;
    g_wf.wBitsPerSample = (WORD)bits_per_sample;
    g_wf.nBlockAlign = g_wf.nChannels * (g_wf.wBitsPerSample / 8);
    g_wf.nAvgBytesPerSec = g_wf.nSamplesPerSec * g_wf.nBlockAlign;

    MMRESULT res = waveOutOpen(&g_hWaveOut, WAVE_MAPPER, &g_wf, (DWORD_PTR)wave_out_proc, 0, CALLBACK_FUNCTION);
    if (res != MMSYSERR_NOERROR) {
        printf("waveOutOpen failed: %d\n", (int)res);
        DeleteCriticalSection(&g_cs);
        return -1;
    }
    g_inited = 1;
    g_played_samples = 0;
    return 0;
}

int audio_play_write(const void* pcm, int bytes, uint64_t pts_ms)
{
    if (!g_inited) return -1;
    if (!pcm || bytes <= 0) return -1;

    /* allocate audio_buf_t and its buffer */
    audio_buf_t* ab = (audio_buf_t*)malloc(sizeof(audio_buf_t));
    if (!ab) return -1;
    memset(ab, 0, sizeof(*ab));
    ab->buf = (uint8_t*)malloc(bytes);
    if (!ab->buf) {
        free(ab);
        return -1;
    }
    memcpy(ab->buf, pcm, bytes);
    ab->bytes = bytes;
    ab->hdr.lpData = (LPSTR)ab->buf;
    ab->hdr.dwBufferLength = bytes;
    ab->hdr.dwFlags = 0;
    ab->hdr.dwUser = (DWORD_PTR)ab;
    /* 每通道样本数：bytes / (channels * bytes_per_sample) */
    ab->samples = bytes / (g_channels * g_bytes_per_sample);
    ab->pts_ms = pts_ms;

    MMRESULT res;
    EnterCriticalSection(&g_cs);
    res = waveOutPrepareHeader(g_hWaveOut, &ab->hdr, sizeof(WAVEHDR));
    if (res != MMSYSERR_NOERROR) {
        LeaveCriticalSection(&g_cs);
        free(ab->buf);
        free(ab);
        return -1;
    }
    res = waveOutWrite(g_hWaveOut, &ab->hdr, sizeof(WAVEHDR));
    LeaveCriticalSection(&g_cs);
    if (res != MMSYSERR_NOERROR) {
        /* cleanup will be done in callback only when written; here free since failed */
        waveOutUnprepareHeader(g_hWaveOut, &ab->hdr, sizeof(WAVEHDR));
        free(ab->buf);
        free(ab);
        return -1;
    }
    return 0;
}

/* 获取当前音频时钟（ms）：
   优化：支持 TIME_SAMPLES / TIME_MS / TIME_BYTES 三种返回类型，回退到已完成的样本计数 */
uint64_t audio_get_clock_ms(void)
{
    if (!g_inited) return 0;

    uint64_t played_samples_snapshot;
    int sample_rate_snapshot;
    int channels_snapshot;
    int bytes_per_sample_snapshot;
    HWAVEOUT hwo_snapshot;

    EnterCriticalSection(&g_cs);
    played_samples_snapshot = g_played_samples;
    sample_rate_snapshot = g_sample_rate;
    channels_snapshot = g_channels;
    bytes_per_sample_snapshot = g_bytes_per_sample;
    hwo_snapshot = g_hWaveOut;
    LeaveCriticalSection(&g_cs);

    if (sample_rate_snapshot <= 0) return 0;

    /* Try to query position from the driver */
    MMTIME mmtime;
    memset(&mmtime, 0, sizeof(mmtime));
    /* prefer samples if possible */
    mmtime.wType = TIME_SAMPLES;
    if (hwo_snapshot && waveOutGetPosition(hwo_snapshot, &mmtime, sizeof(mmtime)) == MMSYSERR_NOERROR) {
        uint64_t samples_from_api = 0;
        if (mmtime.wType == TIME_SAMPLES) {
            samples_from_api = (uint64_t)mmtime.u.sample;
        } else if (mmtime.wType == TIME_MS) {
            /* driver gives milliseconds directly */
            return (uint64_t)mmtime.u.ms;
        } else if (mmtime.wType == TIME_BYTES) {
            /* convert bytes played to samples per channel */
            if (channels_snapshot > 0 && bytes_per_sample_snapshot > 0) {
                samples_from_api = (uint64_t)(mmtime.u.cb) / (uint64_t)(channels_snapshot * bytes_per_sample_snapshot);
            }
        } else {
            /* unknown type: fallback */
            samples_from_api = 0;
        }

        /* Use the maximum of our counted finished samples and API samples */
        if (samples_from_api > played_samples_snapshot) played_samples_snapshot = samples_from_api;

        uint64_t ms = (played_samples_snapshot * 1000ULL) / (uint64_t)sample_rate_snapshot;
        return ms;
    }

    /* If we couldn't query driver position, fallback to counted finished buffers */
    uint64_t ms = (played_samples_snapshot * 1000ULL) / (uint64_t)sample_rate_snapshot;
    return ms;
}

void audio_play_deinit(void)
{
    if (!g_inited) return;
    /* stop and reset */
    waveOutReset(g_hWaveOut);

    /* waveOutClose will wait for buffers to be unprepared if not done; ensure no leaks */
    waveOutClose(g_hWaveOut);
    g_hWaveOut = NULL;

    DeleteCriticalSection(&g_cs);
    g_inited = 0;
}