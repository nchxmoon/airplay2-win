
#pragma once

#include "stream.h"
// 2025.11.13, add by jack, video_process保存成图片/视频.
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


// ffmpeg version & configuration
static void print_ffmpeg_parsers_and_version()
{
	printf("avcodec_version: %d\n", avcodec_version());
	printf("avcodec_configuration: %s\n", avcodec_configuration ? avcodec_configuration() : "(no config func)\n");

	const AVCodecParser* p = NULL;
	int found_h264 = 0;
#if defined(av_parser_iterate) || (LIBAVCODEC_VERSION_MAJOR >= 58)
	void* iter = NULL;
	printf("Available parsers (av_parser_iterate):\n");
	while ((p = av_parser_iterate(&iter))) {
		if (p->codec_ids) {
			for (int i = 0; p->codec_ids[i] != AV_CODEC_ID_NONE; ++i) {
				if (p->codec_ids[i] == AV_CODEC_ID_H264) found_h264 = 1;
			}
		}
	}
#else
	printf("Available parsers (av_parser_next):\n");
	p = av_parser_next(NULL);
	while (p) {
		if (p->codec_ids) {
			for (int i = 0; p->codec_ids[i] != AV_CODEC_ID_NONE; ++i) {
				if (p->codec_ids[i] == AV_CODEC_ID_H264) found_h264 = 1;
			}
		}
		p = av_parser_next(p);
	}
#endif

	if (!found_h264) {
		printf("H.264 parser NOT found in this libavcodec build.\n");
	}
	else {
		printf("H.264 parser found.\n");
	}
}

////////////////////////////////////////// jepg ///////////////////////////////////////////////////
// data -> jpeg
static void save_to_jpeg(h264_decode_struct* data) {
	/* 内部静态 FFmpeg 上下文（懒初始化）*/
	static AVCodecParserContext* parser = NULL;
	static AVCodecContext* dec_ctx = NULL;
	static AVFrame* frame = NULL;
	static struct SwsContext* sws_ctx = NULL;
	static int initialized = 0;
	static unsigned long long frame_count = 0;

	int ret;

	if (!data || !data->data || data->data_len <= 0) {
		printf("video_process: empty data\n");
		return;
	}

	if (!initialized) {
		/* 初始化解码器（第一次调用时）*/
#if LIBAVCODEC_VERSION_MAJOR < 58
		avcodec_register_all();
#endif
		const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) {
			printf("FFmpeg: H264 decoder not found\n");
			return;
		}

		dec_ctx = avcodec_alloc_context3(codec);
		if (!dec_ctx) {
			printf("FFmpeg: could not allocate decoder context\n");
			return;
		}

		if (avcodec_open2(dec_ctx, codec, NULL) < 0) {
			printf("FFmpeg: could not open decoder\n");
			avcodec_free_context(&dec_ctx);
			dec_ctx = NULL;
			return;
		}

		parser = av_parser_init(AV_CODEC_ID_H264);
		if (!parser) {
			printf("FFmpeg: could not init parser\n");

			AVPacket pkt;
			av_init_packet(&pkt);
			pkt.data = data->data;
			pkt.size = data->data_len;
			// 可设置 pts/dts: pkt.pts = data->pts 等（需按解码器 timebase 转换）
			ret = avcodec_send_packet(dec_ctx, &pkt);
			if (ret < 0) {
				printf("avcodec_send_packet (direct) failed: %d\n", ret);
			}
			else {
				while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
					// 处理解码帧（保存为 JPEG 等）
					av_frame_unref(frame);
				}
				if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
					printf("avcodec_receive_frame error: %d\n", ret);
				}
			}

			avcodec_free_context(&dec_ctx);
			dec_ctx = NULL;
			return;
		}

		frame = av_frame_alloc();
		if (!frame) {
			printf("FFmpeg: could not allocate frame\n");
			av_parser_close(parser);
			avcodec_free_context(&dec_ctx);
			dec_ctx = NULL;
			return;
		}

		initialized = 1;
		printf("FFmpeg: video decoder initialized\n");
	}

	/* 使用 parser 将输入流切分成可解码 packet */
	uint8_t* in_data = data->data;
	int in_size = data->data_len;

	while (in_size > 0) {
		uint8_t* out_data = NULL;
		int out_size = 0;
		int consumed = av_parser_parse2(parser, dec_ctx, &out_data, &out_size,
			in_data, in_size,
			(long long)data->pts, AV_NOPTS_VALUE, 0);
		if (consumed < 0) {
			printf("FFmpeg: parser error\n");
			break;
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
				printf("FFmpeg: avcodec_send_packet error: %d\n", ret);
				continue;
			}

			while (ret >= 0) {
				ret = avcodec_receive_frame(dec_ctx, frame);
				if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
					break;
				}
				else if (ret < 0) {
					printf("FFmpeg: avcodec_receive_frame error: %d\n", ret);
					break;
				}

				/* 解码得到一帧：将其转换并用 MJPEG 编码保存为 JPEG 文件 */
				/* 创建 MJPEG 编码器上下文 临时使用然后释放，保持代码简洁 */
				const AVCodec* mjpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
				if (!mjpeg) {
					printf("FFmpeg: MJPEG encoder not found\n");
					continue;
				}

				AVCodecContext* enc_ctx = avcodec_alloc_context3(mjpeg);
				if (!enc_ctx) {
					printf("FFmpeg: failed to alloc mjpeg ctx\n");
					continue;
				}

				enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
				enc_ctx->height = frame->height;
				enc_ctx->width = frame->width;
				enc_ctx->time_base = (AVRational){ 1, 25 };
				/* 可根据需要设置质量：
				   av_opt_set_int(enc_ctx->priv_data, "quality", 5, 0); */

				if (avcodec_open2(enc_ctx, mjpeg, NULL) < 0) {
					printf("FFmpeg: could not open mjpeg encoder\n");
					avcodec_free_context(&enc_ctx);
					continue;
				}

				/* 准备目标 AVFrame，用于编码（格式为 enc_ctx->pix_fmt）*/
				AVFrame* frame_rgb = av_frame_alloc();
				if (!frame_rgb) {
					avcodec_free_context(&enc_ctx);
					printf("FFmpeg: failed to alloc tmp frame\n");
					continue;
				}
				frame_rgb->format = enc_ctx->pix_fmt;
				frame_rgb->width = enc_ctx->width;
				frame_rgb->height = enc_ctx->height;

				ret = av_image_alloc(frame_rgb->data, frame_rgb->linesize,
					frame_rgb->width, frame_rgb->height,
					enc_ctx->pix_fmt, 32);
				if (ret < 0) {
					printf("FFmpeg: av_image_alloc failed\n");
					av_frame_free(&frame_rgb);
					avcodec_free_context(&enc_ctx);
					continue;
				}

				/* 如果需要，创建或更新 sws_ctx 将 decode 输出转换到 MJPEG 要求的像素格式 */
				sws_ctx = sws_getContext(frame->width, frame->height, dec_ctx->pix_fmt,
					frame_rgb->width, frame_rgb->height, enc_ctx->pix_fmt,
					SWS_BILINEAR, NULL, NULL, NULL);
				if (!sws_ctx) {
					printf("FFmpeg: sws_getContext failed\n");
					av_freep(&frame_rgb->data[0]);
					av_frame_free(&frame_rgb);
					avcodec_free_context(&enc_ctx);
					continue;
				}

				/* 执行颜色空间/像素格式转换 */
				sws_scale(sws_ctx, (const uint8_t* const*)frame->data, frame->linesize,
					0, frame->height, frame_rgb->data, frame_rgb->linesize);

				/* 编码为 JPEG */
				AVPacket enc_pkt;
				av_init_packet(&enc_pkt);
				enc_pkt.data = NULL;
				enc_pkt.size = 0;

				ret = avcodec_send_frame(enc_ctx, frame_rgb);
				if (ret < 0) {
					printf("FFmpeg: send_frame to mjpeg failed: %d\n", ret);
				}
				else {
					ret = avcodec_receive_packet(enc_ctx, &enc_pkt);
					if (ret == 0) {
						char filename[256];
						/* 使用 pts 与自增计数保证文件名唯一 */
						snprintf(filename
							, sizeof(filename)
							, "%s\\pic\\frame_%llu_%06llu.jpg"
							, SNAP_SHOT_PATH
							, (unsigned long long)data->pts
							, frame_count++
						);
						FILE* f = fopen(filename, "wb");
						if (f) {
							fwrite(enc_pkt.data, 1, enc_pkt.size, f);
							fclose(f);
							printf("Saved frame as %s (%d bytes)\n", filename, enc_pkt.size);
						}
						else {
							perror("fopen");
						}
						av_packet_unref(&enc_pkt);
					}
					else {
						printf("FFmpeg: receive_packet from mjpeg failed: %d\n", ret);
					}
				}

				/* 释放资源 */
				sws_freeContext(sws_ctx);
				sws_ctx = NULL;
				av_freep(&frame_rgb->data[0]);
				av_frame_free(&frame_rgb);
				avcodec_free_context(&enc_ctx);

				/* 释放解码帧数据（FFmpeg 管理）并继续接收下一帧（如果有） */
			} /* end receive_frame loop */
		} /* end if out_size > 0 */
	} /* end while in_size */

	/* NOTE:
	   - 当前实现对 FFmpeg 上下文使用懒初始化且非线程安全。
	   - 建议将初始化/销毁逻辑移到独立函数，在程序启动/退出时明确调用，
		 并在多线程场景中对解码器访问加锁。
	*/
}

////////////////////////////////////////// video ///////////////////////////////////////////////////
/* ---- 持久化输出上下文 ---- */
static AVFormatContext* out_fmt_ctx = NULL;
static AVStream* out_stream = NULL;
static AVCodecContext* video_enc_ctx = NULL;
static AVFrame* enc_frame = NULL;
static struct SwsContext* to_enc_sws = NULL;
static int64_t enc_next_pts = 0;

/* 初始化输出文件（在第一次有帧时调用）*/
static int ffmpeg_video_init(int width, int height, const char* filename)
{
	int ret;
	AVOutputFormat* ofmt = NULL;
	AVCodec* codec = NULL;

	if (out_fmt_ctx) return 0; /* 已初始化 */

	avformat_alloc_output_context2(&out_fmt_ctx, NULL, NULL, filename);
	if (!out_fmt_ctx) {
		printf("FFmpeg: could not alloc output context\n");
		return -1;
	}
	ofmt = out_fmt_ctx->oformat;

	/* 使用 MJPEG 编码器（容器 AVI 常用）*/
	codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (!codec) {
		printf("FFmpeg: MJPEG encoder not found\n");
		return -1;
	}

	out_stream = avformat_new_stream(out_fmt_ctx, NULL);
	if (!out_stream) {
		printf("FFmpeg: could not create stream\n");
		return -1;
	}

	video_enc_ctx = avcodec_alloc_context3(codec);
	if (!video_enc_ctx) {
		printf("FFmpeg: could not alloc enc ctx\n");
		return -1;
	}

	/* 设置编码参数 */
	video_enc_ctx->codec_id = AV_CODEC_ID_MJPEG;
	video_enc_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
	video_enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
	//video_enc_ctx->pix_fmt = AV_PIX_FMT_YUV420P; /* 不使用已弃用 YUVJ */
	video_enc_ctx->width = width;
	video_enc_ctx->height = height;
	video_enc_ctx->time_base = (AVRational){ 1, 25 };
	video_enc_ctx->gop_size = 12;
	video_enc_ctx->bit_rate = 4000000;
	video_enc_ctx->color_range = AVCOL_RANGE_JPEG; /* 表示 full range（JPEG） */

	/* 有些 muxer 要求 global headers */
	if (out_fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		video_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(video_enc_ctx, codec, NULL);
	if (ret < 0) {
		char errbuf[128];
		av_strerror(ret, errbuf, sizeof(errbuf));
		printf("FFmpeg: avcodec_open2 failed: %s\n", errbuf);
		return -1;
	}

	ret = avcodec_parameters_from_context(out_stream->codecpar, video_enc_ctx);
	if (ret < 0) {
		printf("FFmpeg: avcodec_parameters_from_context failed\n");
		return -1;
	}
	out_stream->time_base = video_enc_ctx->time_base;

	/* 打开输出文件 */
	if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&out_fmt_ctx->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0) {
			printf("FFmpeg: avio_open failed\n");
			return -1;
		}
	}

	/* 写文件头 */
	ret = avformat_write_header(out_fmt_ctx, NULL);
	if (ret < 0) {
		printf("FFmpeg: avformat_write_header failed\n");
		return -1;
	}

	/* 为目标编码帧分配 AVFrame */
	enc_frame = av_frame_alloc();
	if (!enc_frame) {
		printf("FFmpeg: could not alloc enc_frame\n");
		return -1;
	}
	enc_frame->format = video_enc_ctx->pix_fmt;
	enc_frame->width = video_enc_ctx->width;
	enc_frame->height = video_enc_ctx->height;
	ret = av_image_alloc(enc_frame->data, enc_frame->linesize, enc_frame->width, enc_frame->height, video_enc_ctx->pix_fmt, 32);
	if (ret < 0) {
		printf("FFmpeg: av_image_alloc failed\n");
		return -1;
	}

	enc_next_pts = 0;
	printf("FFmpeg: output initialized: %s (%dx%d)\n", filename, width, height);
	return 0;
}

/* 写入一帧（已转换到 enc_frame 的像素格式）*/
static int ffmpeg_video_write_frame(AVFrame* frame)
{
	int ret;
	AVPacket pkt;
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	/* 设 pts（以编码器 time_base） */
	frame->pts = enc_next_pts++;

	ret = avcodec_send_frame(video_enc_ctx, frame);
	if (ret < 0) {
		printf("FFmpeg: avcodec_send_frame error: %d\n", ret);
		return ret;
	}
	while (ret >= 0) {
		ret = avcodec_receive_packet(video_enc_ctx, &pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
		else if (ret < 0) {
			printf("FFmpeg: avcodec_receive_packet error: %d\n", ret);
			break;
		}
		/* 将 packet 写入容器，时间基转换交由 muxer/stream 管理 */
		pkt.stream_index = out_stream->index;
		av_packet_rescale_ts(&pkt, video_enc_ctx->time_base, out_stream->time_base);
		ret = av_interleaved_write_frame(out_fmt_ctx, &pkt);
		av_packet_unref(&pkt);
		if (ret < 0) {
			printf("FFmpeg: av_interleaved_write_frame failed: %d\n", ret);
			break;
		}
	}
	return 0;
}

/* 释放并写入尾部 */
static void ffmpeg_video_deinit()
{
	if (!out_fmt_ctx) return;

	/* 刷新编码器 */
	if (video_enc_ctx) {
		avcodec_send_frame(video_enc_ctx, NULL);
		AVPacket pkt;
		av_init_packet(&pkt);
		while (avcodec_receive_packet(video_enc_ctx, &pkt) == 0) {
			pkt.stream_index = out_stream->index;
			av_packet_rescale_ts(&pkt, video_enc_ctx->time_base, out_stream->time_base);
			av_interleaved_write_frame(out_fmt_ctx, &pkt);
			av_packet_unref(&pkt);
		}
	}

	av_write_trailer(out_fmt_ctx);

	if (to_enc_sws) {
		sws_freeContext(to_enc_sws);
		to_enc_sws = NULL;
	}
	if (enc_frame) {
		av_freep(&enc_frame->data[0]);
		av_frame_free(&enc_frame);
		enc_frame = NULL;
	}
	if (video_enc_ctx) {
		avcodec_free_context(&video_enc_ctx);
		video_enc_ctx = NULL;
	}
	if (out_fmt_ctx) {
		if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
			avio_closep(&out_fmt_ctx->pb);
		avformat_free_context(out_fmt_ctx);
		out_fmt_ctx = NULL;
	}
	printf("FFmpeg: output closed\n");
}

/* 将接收到的 h264 data 解码并写入视频（lazy init 输出） */
static void save_to_video(h264_decode_struct* data)
{
	/* 内部静态 解码上下文（和 parser） */
	static AVCodecParserContext* parser = NULL;
	static AVCodecContext* dec_ctx = NULL;
	static AVFrame* frame = NULL;
	static int initialized = 0;
	static unsigned long long frame_count = 0;

	int ret;

	if (!data || !data->data || data->data_len <= 0) return;

	if (!initialized) {
#if LIBAVCODEC_VERSION_MAJOR < 58
		avcodec_register_all();
#endif
		const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		if (!codec) {
			printf("FFmpeg: H264 decoder not found\n");
			return;
		}
		dec_ctx = avcodec_alloc_context3(codec);
		if (!dec_ctx) {
			printf("FFmpeg: could not allocate decoder context\n");
			return;
		}
		if (avcodec_open2(dec_ctx, codec, NULL) < 0) {
			printf("FFmpeg: could not open decoder\n");
			avcodec_free_context(&dec_ctx);
			return;
		}
		parser = av_parser_init(AV_CODEC_ID_H264);
		if (!parser) {
			printf("FFmpeg: parser not available, cannot reliably parse stream\n");
			/* 仍可尝试直接送包，但行为取决于包边界 */
		}
		frame = av_frame_alloc();
		if (!frame) {
			printf("FFmpeg: could not allocate frame\n");
			if (parser) av_parser_close(parser);
			avcodec_free_context(&dec_ctx);
			return;
		}
		initialized = 1;
	}

	/* parse + decode loop */
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
				printf("FFmpeg: parser error\n");
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
				//printf("avcodec_send_packet error: %d\n", ret);
				av_packet_unref(&pkt);
				continue;
			}
			while ((ret = avcodec_receive_frame(dec_ctx, frame)) >= 0) {
				/* 当首次获得帧时，如果输出未初始化则进行初始化 */
				if (!out_fmt_ctx) {
					char outfilename[256];
					/* 使用 pts 与自增计数保证文件名唯一 */
					snprintf(outfilename
						, sizeof(outfilename)
						, "%s\\avi\\output_%llu_%06llu.avi"
						, SNAP_SHOT_PATH
						, (unsigned long long)data->pts
						, frame_count++
					);
					if (ffmpeg_video_init(frame->width, frame->height, outfilename) < 0) {
						printf("FFmpeg: failed to initialize output\n");
						av_frame_unref(frame);
						continue;
					}
				}

				/* 确保 sws_ctx 可用并转换到编码器像素格式 */
				enum AVPixelFormat src_pix = (enum AVPixelFormat)frame->format;
				enum AVPixelFormat dst_pix = video_enc_ctx->pix_fmt;
				if (!to_enc_sws) {
					to_enc_sws = sws_getContext(frame->width, frame->height, src_pix,
						video_enc_ctx->width, video_enc_ctx->height, dst_pix,
						SWS_BILINEAR, NULL, NULL, NULL);
					if (!to_enc_sws) {
						printf("FFmpeg: sws_getContext failed\n");
						av_frame_unref(frame);
						continue;
					}
				}

				/* 转换到 enc_frame 的缓冲区 */
				sws_scale(to_enc_sws, (const uint8_t* const*)frame->data, frame->linesize,
					0, frame->height, enc_frame->data, enc_frame->linesize);
				enc_frame->pts = enc_next_pts;

				/* 标记 full-range */
				enc_frame->color_range = AVCOL_RANGE_JPEG;

				/* 写入编码器并复用 */
				ffmpeg_video_write_frame(enc_frame);

				av_frame_unref(frame);
			}
			if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF && ret < 0) {
				printf("FFmpeg: avcodec_receive_frame error: %d\n", ret);
			}
			av_packet_unref(&pkt);
		}
	} /* end while in_size */
}
