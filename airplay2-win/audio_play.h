#pragma once
#include <stdint.h>
#include "stream.h"

/* 初始化音频播放
   sample_rate: 44100, 48000...
   channels: 1 或 2
   bits_per_sample: 16 (本实现假设 16)
   返回 0 表示成功 */
int audio_play_init(int sample_rate, int channels, int bits_per_sample);

/* 把一段 PCM 音频加入播放队列
   pcm: 指向 PCM 数据（16-bit interleaved 或符合你的 data 格式）
   bytes: 字节长度
   pts_ms: 该缓冲区开始对应的时间戳（毫秒） — 必须与视频 PTS 的时间基一致
   返回 0 表示成功 */
int audio_play_write(const void* pcm, int bytes, uint64_t pts_ms);

/* 返回当前音频播放时钟（毫秒）相对于同一时间基（例如视频 PTS 单位为毫秒） */
uint64_t audio_get_clock_ms(void);

uint64_t audio_pts_to_ms(const pcm_data_struct* a);

/* 释放资源 */
void audio_play_deinit(void);