/*
 * FILE:    video_decompress/dxt_glsl.c
 * AUTHORS: Martin Benes     <martinbenesh@gmail.com>
 *          Lukas Hejtmanek  <xhejtman@ics.muni.cz>
 *          Petr Holub       <hopet@ics.muni.cz>
 *          Milos Liska      <xliska@fi.muni.cz>
 *          Jiri Matela      <matela@ics.muni.cz>
 *          Dalibor Matura   <255899@mail.muni.cz>
 *          Ian Wesley-Smith <iwsmith@cct.lsu.edu>
 *
 * Copyright (c) 2005-2011 CESNET z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, is permitted provided that the following conditions
 * are met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 * 
 *      This product includes software developed by CESNET z.s.p.o.
 * 
 * 4. Neither the name of the CESNET nor the names of its contributors may be
 *    used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#include "config_unix.h"
#include "config_win32.h"
#endif // HAVE_CONFIG_H

#include "video_decompress/libavcodec.h"

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

#include "debug.h"
#include "video_decompress.h"

struct state_libavcodec_decompress {
        AVCodec         *codec;
        AVCodecContext  *codec_ctx;
        AVFrame         *frame;
        AVPacket         pkt;

        int              width, height;
        int              pitch;
        int              rshift, gshift, bshift;
        int              max_compressed_len;
        codec_t          in_codec;
        codec_t          out_codec;

        int              last_frame_seq;
};

static void yuv420p_to_yuv422(char *dst_buffer, AVFrame *in_frame,
                int width, int height);
static void yuv422p_to_yuv422(char *dst_buffer, AVFrame *in_frame,
                int width, int height);
static void yuv422p_to_rgb24(char *dst_buffer, AVFrame *in_frame,
                int width, int height);
static int change_pixfmt(AVFrame *frame, unsigned char *dst, int av_codec,
                codec_t out_codec, int width, int height);

static void deconfigure(struct state_libavcodec_decompress *s)
{
        if(s->codec_ctx)
                avcodec_close(s->codec_ctx);
        av_free(s->codec_ctx);
        av_free(s->frame);
        av_free_packet(&s->pkt);
}

static bool configure_with(struct state_libavcodec_decompress *s,
                struct video_desc desc)
{
        int codec_id;
        switch(desc.color_spec) {
                case H264:
                        codec_id = CODEC_ID_H264;
                        break;
                case MJPG:
                case JPEG:
                        codec_id = CODEC_ID_MJPEG;
                        fprintf(stderr, "[lavd] Warning: JPEG decoder "
                                        "will use full-scale YUV.\n");
                        break;
                case VP8:
                        codec_id = CODEC_ID_VP8;
                        break;
                default:
                        fprintf(stderr, "[lavd] Unsupported codec!!!\n");
                        return false;
        }

        s->codec = avcodec_find_decoder(codec_id);
        if(s->codec == NULL) {
                fprintf(stderr, "[lavd] Unable to find codec.\n");
                return false;
        }

        s->codec_ctx = avcodec_alloc_context3(s->codec);
        if(s->codec_ctx == NULL) {
                fprintf(stderr, "[lavd] Unable to allocate codec context.\n");
                return false;
        }

        // zero should mean count equal to the number of virtual cores
        if(s->codec->capabilities & CODEC_CAP_SLICE_THREADS) {
                s->codec_ctx->thread_count = 0;
                s->codec_ctx->thread_type = FF_THREAD_SLICE;
        } else {
                fprintf(stderr, "[lavd] Warning: Codec doesn't support slice-based multithreading.\n");
#if 0
                if(s->codec->capabilities & CODEC_CAP_FRAME_THREADS) {
                        s->codec_ctx->thread_count = 0;
                        s->codec_ctx->thread_type = FF_THREAD_FRAME;
                } else {
                        fprintf(stderr, "[lavd] Warning: Codec doesn't support frame-based multithreading.\n");
                }
#endif
        }

        s->codec_ctx->pix_fmt = PIX_FMT_YUV420P;

        if(avcodec_open2(s->codec_ctx, s->codec, NULL) < 0) {
                fprintf(stderr, "[lavd] Unable to open decoder.\n");
                return false;
        }

        s->frame = avcodec_alloc_frame();
        if(!s->frame) {
                fprintf(stderr, "[lavd] Unable allocate frame.\n");
                return false;
        }

        av_init_packet(&s->pkt);

        s->last_frame_seq = -1;

        return true;
}

void * libavcodec_decompress_init(void)
{
        struct state_libavcodec_decompress *s;
        
        s = (struct state_libavcodec_decompress *)
                malloc(sizeof(struct state_libavcodec_decompress));

        /*   register all the codecs (you can also register only the codec
         *         you wish to have smaller code */
        avcodec_register_all();

        s->width = s->height = s->pitch = 0;
        s->codec_ctx = NULL;;
        s->frame = NULL;
        av_init_packet(&s->pkt);
        s->pkt.data = NULL;
        s->pkt.size = 0;

        return s;
}

int libavcodec_decompress_reconfigure(void *state, struct video_desc desc, 
                int rshift, int gshift, int bshift, int pitch, codec_t out_codec)
{
        struct state_libavcodec_decompress *s =
                (struct state_libavcodec_decompress *) state;
        
        // assume that we have the same pitch as line size, for now
        assert(pitch == vc_get_linesize(desc.width, out_codec));
        // TODO: add also RGB output codecs if possible
        assert(out_codec == UYVY || out_codec == RGB);

        s->pitch = pitch;
        s->rshift = rshift;
        s->gshift = gshift;
        s->bshift = bshift;
        s->in_codec = desc.color_spec;
        s->out_codec = out_codec;
        s->width = desc.width;
        s->height = desc.height;

        deconfigure(s);
        configure_with(s, desc);

        s->max_compressed_len = 4 * desc.width * desc.height;

        return s->max_compressed_len;
}


static void yuv420p_to_yuv422(char *dst_buffer, AVFrame *in_frame,
                int width, int height)
{
        for(int y = 0; y < (int) height; ++y) {
                char *src = (char *) in_frame->data[0] + in_frame->linesize[0] * y;
                char *dst = (char *) dst_buffer + width * y * 2;
                for(int x = 0; x < width; ++x) {
                        dst[x * 2 + 1] = src[x];
                }
        }

        for(int y = 0; y < (int) height / 2; ++y) {
                char *src_cb = (char *) in_frame->data[1] + in_frame->linesize[1] * y;
                char *src_cr = (char *) in_frame->data[2] + in_frame->linesize[2] * y;
                char *dst1 = dst_buffer + width * (y * 2) * 2;
                char *dst2 = dst_buffer + (y * 2 + 1) * width * 2;
                for(int x = 0; x < width / 2; ++x) {
                        dst1[x * 4] = src_cb[x];
                        dst1[x * 4 + 2] = src_cr[x];
                        dst2[x * 4] = src_cb[x];
                        dst2[x * 4 + 2] = src_cr[x];
                }
        }
}

static void yuv422p_to_yuv422(char *dst_buffer, AVFrame *in_frame,
                int width, int height)
{
        for(int y = 0; y < (int) height; ++y) {
                char *src = (char *) in_frame->data[0] + in_frame->linesize[0] * y;
                char *dst = (char *) dst_buffer + width * y * 2;
                for(int x = 0; x < width; ++x) {
                        dst[x * 2 + 1] = src[x];
                }
        }

        for(int y = 0; y < (int) height; ++y) {
                char *src_cb = (char *) in_frame->data[1] + in_frame->linesize[1] * y;
                char *src_cr = (char *) in_frame->data[2] + in_frame->linesize[2] * y;
                char *dst = dst_buffer + width * y * 2;
                for(int x = 0; x < width / 2; ++x) {
                        dst[x * 4] = src_cb[x];
                        dst[x * 4 + 2] = src_cr[x];
                }
        }
}

/**
 * Changes pixel format from planar YUV 422 to packed RGB.
 * Color space is assumed ITU-T Rec. 609. YUV is expected to be full scale (aka in JPEG).
 */
static void yuv422p_to_rgb24(char *dst_buffer, AVFrame *in_frame,
                int width, int height)
{
        for(int y = 0; y < (int) height; ++y) {
                unsigned char *src_y = (unsigned char *) in_frame->data[0] + in_frame->linesize[0] * y;
                unsigned char *src_cb = (unsigned char *) in_frame->data[1] + in_frame->linesize[1] * y;
                unsigned char *src_cr = (unsigned char *) in_frame->data[2] + in_frame->linesize[2] * y;
                unsigned char *dst = (unsigned char *) dst_buffer + width * y * 3;
                for(int x = 0; x < width / 2; ++x) {
                        int cb = *src_cb++ - 128;
                        int cr = *src_cr++ - 128;
                        int y = *src_y++ << 16;
                        int r = 75700 * cr;
                        int g = -26864 * cb - 38050 * cr;
                        int b = 133176 * cb;
                        *dst++ = min(max(r + y, 0), (1<<24) - 1) >> 16;
                        *dst++ = min(max(g + y, 0), (1<<24) - 1) >> 16;
                        *dst++ = min(max(b + y, 0), (1<<24) - 1) >> 16;
                        y = *src_y++ << 16;
                        *dst++ = min(max(r + y, 0), (1<<24) - 1) >> 16;
                        *dst++ = min(max(g + y, 0), (1<<24) - 1) >> 16;
                        *dst++ = min(max(b + y, 0), (1<<24) - 1) >> 16;
                }
        }
}

/**
 * Changes pixel format from frame to native (currently UYVY).
 *
 * @todo             figure out color space transformations - eg. JPEG returns full-scale YUV.
 *                   And not in the ITU-T Rec. 701 (eventually Rec. 609) scale.
 * @param  frame     video frame returned from libavcodec decompress
 * @param  dst       destination buffer where data will be stored
 * @param  av_codec  libav pixel format
 * @param  out_codec requested output codec
 * @param  width     frame width
 * @param  height    frame height
 * @retval TRUE      if the transformation was successful
 * @retval FALSE     if transformation failed
 * @see    yuvj422p_to_yuv422
 * @see    yuv420p_to_yuv422
 */
static int change_pixfmt(AVFrame *frame, unsigned char *dst, int av_codec,
                codec_t out_codec, int width, int height) {
        assert(out_codec == UYVY || out_codec == RGB);

        switch(av_codec) {
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                case AV_PIX_FMT_YUV422P:
                case AV_PIX_FMT_YUVJ422P:
#else
                case PIX_FMT_YUV422P:
                case PIX_FMT_YUVJ422P:
#endif
                        if(out_codec == UYVY) {
                                yuv422p_to_yuv422((char *) dst, frame, width, height);
                        } else {
                                yuv422p_to_rgb24((char *) dst, frame, width, height);
                        }
                        break;
#ifdef HAVE_AVCODEC_ENCODE_VIDEO2
                case AV_PIX_FMT_YUV420P:
                case AV_PIX_FMT_YUVJ420P:
#else
                case PIX_FMT_YUV420P:
                case PIX_FMT_YUVJ420P:
#endif
                        assert(out_codec == UYVY);
                        yuv420p_to_yuv422((char *) dst, frame, width, height);
                        break;
                default:
                        fprintf(stderr, "Unsupported pixel "
                                        "format: %s (id %d)\n",
                                        av_get_pix_fmt_name(
                                                av_codec), av_codec);
                        return FALSE;
        }
        return TRUE;
}

int libavcodec_decompress(void *state, unsigned char *dst, unsigned char *src,
                unsigned int src_len, int frame_seq)
{
        struct state_libavcodec_decompress *s = (struct state_libavcodec_decompress *) state;
        int len, got_frame;
        int res = FALSE;

        s->pkt.size = src_len;
        s->pkt.data = src;
        
        while (s->pkt.size > 0) {
                len = avcodec_decode_video2(s->codec_ctx, s->frame, &got_frame, &s->pkt);

                /*
                 * Hack: libavcodec does not correctly support JPEG with more than one reset
                 * segment. It returns error although it is actually able to decompress frame
                 * correctly. So we assume that the decompression went good even with the 
                 * reported error.
                 */
                if(len < 0 && s->in_codec == JPEG) {
                        return change_pixfmt(s->frame, dst, s->codec_ctx->pix_fmt,
                                        s->out_codec, s->width, s->height);
                }

                if(len < 0) {
                        fprintf(stderr, "[lavd] Error while decoding frame.\n");
                        return FALSE;
                }

                if(got_frame) {
                        /* pass frame only if this is I-frame or we have complete
                         * GOP (assuming we are not using B-frames */
                        if(s->frame->pict_type == AV_PICTURE_TYPE_I ||
                                        (s->frame->pict_type == AV_PICTURE_TYPE_P &&
                                         s->last_frame_seq == frame_seq - 1)
                                        ) {
                                res = change_pixfmt(s->frame, dst, s->codec_ctx->pix_fmt,
                                                s->out_codec, s->width, s->height);
                                if(res == TRUE) {
                                        s->last_frame_seq = frame_seq;
                                }
                        } else {
                                fprintf(stderr, "[lavd] Missing appropriate I-frame "
                                                "(last valid %d, this %d).\n", s->last_frame_seq,
                                                frame_seq);
                                res = FALSE;
                        }
                }

                if(s->pkt.data) {
                        s->pkt.size -= len;
                        s->pkt.data += len;
                }
        }

        return res;
}

int libavcodec_decompress_get_property(void *state, int property, void *val, size_t *len)
{
        struct state_libavcodec_decompress *s =
                (struct state_libavcodec_decompress *) state;
        UNUSED(s);
        int ret = FALSE;

        switch(property) {
                case DECOMPRESS_PROPERTY_ACCEPTS_CORRUPTED_FRAME:
                        if(*len >= sizeof(int)) {
                                *(int *) val = FALSE;
                                *len = sizeof(int);
                                ret = TRUE;
                        }
                        break;
                default:
                        ret = FALSE;
        }

        return ret;
}

void libavcodec_decompress_done(void *state)
{
        struct state_libavcodec_decompress *s =
                (struct state_libavcodec_decompress *) state;

        deconfigure(s);

        free(s);
}

