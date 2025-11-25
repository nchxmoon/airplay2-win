#pragma once
#include "audio_play.h"
#include "stream.h"
#ifdef WIN32
#include <windows.h>
#endif

// FFmpeg headers
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>

#define SNAP_SHOT_PATH "D:\\fys\\source\\windows\\airplay\\airplay2-win\\x64\\Debug\\snapshoot"

/* --- 之前的 save_to_jpeg / save_to_video 保留 （如有需要可复用） --- */

/* ---- Win32 实时播放器（GDI） ---- */
static HWND player_hwnd = NULL;
static HDC player_hdc = NULL;
static int player_w = 0;
static int player_h = 0;
static uint8_t* player_rgb = NULL; /* BGR24 buffer */
static struct SwsContext* player_sws = NULL;

/* 创建一个简单窗口用于显示视频 */
static int ffmpeg_player_init_window(int width, int height)
{
	if (player_hwnd) return 0;
#ifdef WIN32
	/* 使用现有窗口类 "STATIC" 快速创建窗口 */
	player_hwnd = CreateWindowA("STATIC", "AirPlay Video",
		WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		CW_USEDEFAULT, CW_USEDEFAULT,
		width + 16, height + 39,
		NULL, NULL, NULL, NULL);
	if (!player_hwnd) {
		printf("CreateWindowA failed\n");
		player_hwnd = NULL;
		return -1;
	}
	player_hdc = GetDC(player_hwnd);
	player_w = width;
	player_h = height;
	/* allocate BGR buffer */
	size_t bufsize = (size_t)player_w * (size_t)player_h * 3;
	player_rgb = (uint8_t*)malloc(bufsize);
	if (!player_rgb) {
		printf("malloc player_rgb failed\n");
		ReleaseDC(player_hwnd, player_hdc);
		DestroyWindow(player_hwnd);
		player_hwnd = NULL;
		player_hdc = NULL;
		return -1;
	}
	memset(player_rgb, 0, bufsize);
	return 0;
#else
	return -1;
#endif
}

/* 在窗口绘制一帧（BGR24 数据） */
static void ffmpeg_player_draw_bgr24(uint8_t* bgr, int linesize)
{
#ifdef WIN32
	if (!player_hwnd || !player_hdc) return;

	BITMAPINFO bmi;
	memset(&bmi, 0, sizeof(bmi));
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = player_w;
	bmi.bmiHeader.biHeight = -player_h; /* top-down */
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 24;
	bmi.bmiHeader.biCompression = BI_RGB;

	/* StretchDIBits 支持不同 linesize，直接传入 bgr */
	StretchDIBits(player_hdc,
		0, 0, player_w, player_h,
		0, 0, player_w, player_h,
		bgr, &bmi, DIB_RGB_COLORS, SRCCOPY);
	UpdateWindow(player_hwnd);
#endif
}

/* 关闭播放器 */
static void ffmpeg_player_deinit()
{
#ifdef WIN32
	if (player_rgb) {
		free(player_rgb);
		player_rgb = NULL;
	}
	if (player_sws) {
		sws_freeContext(player_sws);
		player_sws = NULL;
	}
	if (player_hdc && player_hwnd) {
		ReleaseDC(player_hwnd, player_hdc);
		player_hdc = NULL;
	}
	if (player_hwnd) {
		DestroyWindow(player_hwnd);
		player_hwnd = NULL;
	}
	player_w = player_h = 0;
#endif
}

/* 将解码后的 AVFrame 转为 BGR24 并绘制 */
static void ffmpeg_player_render_frame(AVFrame* frame)
{
	if (!frame) return;
	int w = frame->width;
	int h = frame->height;

	/* 初始化窗口与缓存（首次帧） */
	if (!player_hwnd) {
		if (ffmpeg_player_init_window(w, h) < 0) {
			printf("ffmpeg_player_init_window failed\n");
			return;
		}
	}

	/* 创建/复用 sws 上下文 */
	enum AVPixelFormat src_pix = (enum AVPixelFormat)frame->format;
	enum AVPixelFormat dst_pix = AV_PIX_FMT_BGR24; /* GDI 使用 BGR24 */
	if (!player_sws) {
		player_sws = sws_getContext(w, h, src_pix, player_w, player_h, dst_pix,
			SWS_BILINEAR, NULL, NULL, NULL);
		if (!player_sws) {
			printf("sws_getContext failed\n");
			return;
		}
	}

	/* 目标数组指针与行距 */
	uint8_t* dst_data[4] = { 0 };
	int dst_linesize[4] = { 0 };
	av_image_fill_arrays(dst_data, dst_linesize, player_rgb, dst_pix, player_w, player_h, 1);

	/* 转换并绘制 */
	sws_scale(player_sws, (const uint8_t* const*)frame->data, frame->linesize,
		0, frame->height, dst_data, dst_linesize);

	ffmpeg_player_draw_bgr24(player_rgb, dst_linesize[0]);
}

/* --- 实时播放主流程：解码 h264 data 并渲染到窗口 --- */
static void play_video_realtime(h264_decode_struct* data)
{
	/* 内部静态 解码上下文 */
	static AVCodecParserContext* parser = NULL;
	static AVCodecContext* dec_ctx = NULL;
	static AVFrame* frame = NULL;
	static int initialized = 0;

	int ret;
	if (!data || !data->data || data->data_len <= 0) return;

	if (!initialized) {
#if LIBAVCODEC_VERSION_MAJOR < 58
		avcodec_register_all();
#endif
		const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) {
			printf("H264 decoder not found\n");
			return;
		}
		dec_ctx = avcodec_alloc_context3(codec);
		if (!dec_ctx) {
			printf("could not alloc decoder context\n");
			return;
		}
		if (avcodec_open2(dec_ctx, codec, NULL) < 0) {
			printf("could not open decoder\n");
			avcodec_free_context(&dec_ctx);
			return;
		}
		parser = av_parser_init(AV_CODEC_ID_H264); /* 可能为 NULL（在某些精简构建） */
		frame = av_frame_alloc();
		if (!frame) {
			printf("could not alloc frame\n");
			if (parser) av_parser_close(parser);
			avcodec_free_context(&dec_ctx);
			return;
		}
		initialized = 1;
	}

	uint8_t* in_data = data->data;
	int in_size = data->data_len;
	while (in_size > 0) {
		uint8_t* out_data = NULL;
		int out_size = 0;
		int consumed = 0;
		if (parser) {
			consumed = av_parser_parse2(parser, dec_ctx, &out_data, &out_size,
				in_data, in_size, (long long)data->pts, AV_NOPTS_VALUE, 0);
			if (consumed < 0) {
				printf("parser error\n");
				break;
			}
		}
		else {
			/* 无 parser 回退：把整块作为一个 packet 送入解码器 */
			out_data = in_data;
			out_size = in_size;
			consumed = in_size;
		}
		in_data += consumed;
		in_size -= consumed;

		if (out_size > 0) {
			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.data = out_data;
			pkt.size = out_size;
			ret = avcodec_send_packet(dec_ctx, &pkt);
			if (ret < 0) {
				// 发送失败，跳过
				av_packet_unref(&pkt);
				continue;
			}
			while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
				/* ---- 同步逻辑开始 ---- */
			 //  /* 获取帧对应时间（优先使用外部 data->pts 作为毫秒基准） */
				//uint64_t frame_pts_ms = (uint64_t)data->pts; /* 假设 data->pts 为毫秒 */
				///* 获取当前音频时钟（毫秒）*/
				//uint64_t audio_ms = audio_get_clock_ms();

				//if (frame_pts_ms > audio_ms) {
				//	/* 等待音频时间追上帧时间，分片等待以保持响应 */
				//	uint64_t wait_ms = frame_pts_ms - audio_ms;
				//	while (wait_ms > 2) {
				//		/* 限制最长单次 Sleep，保持响应 */
				//		uint32_t s = (wait_ms > 20) ? 10 : (uint32_t)wait_ms;
				//		Sleep(s);
				//		audio_ms = audio_get_clock_ms();
				//		if (frame_pts_ms <= audio_ms) break;
				//		wait_ms = frame_pts_ms - audio_ms;
				//	}
				//}
				//else {
				//	/* audio 已领先，若超出阈值则丢帧以追赶（避免音画不同步） */
				//	const uint64_t drop_threshold_ms = 200; /* 可调 */
				//	if (audio_ms > frame_pts_ms + drop_threshold_ms) {
				//		/* 丢帧 */
				//		av_frame_unref(frame);
				//		continue;
				//	}
				//}
				///* ---- 同步逻辑结束 ---- */

				/* 将解码帧渲染到窗口 */
				ffmpeg_player_render_frame(frame);
				av_frame_unref(frame);
			}
			if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && ret < 0) {
				printf("avcodec_receive_frame error: %d\n", ret);
			}
			av_packet_unref(&pkt);
		}
	}
}

/* 导出销毁函数，main 退出时调用 */
static void ffmpeg_player_shutdown()
{
	ffmpeg_player_deinit();
}
