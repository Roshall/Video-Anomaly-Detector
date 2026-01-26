//
// Created by lg on 11/7/25.
//
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <filesystem>
#include "painter_text.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}


int test_draw_legend_on_first_frame(const std::string& video_path,
                                    const std::string& font_path,
                                    const std::string& out_yuv_path)
{
    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0)
        throw std::runtime_error("Cannot open input video");

    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0)
        throw std::runtime_error("Cannot find stream info");

    int vid_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vid_idx < 0) throw std::runtime_error("No video stream found");

    AVStream* vs = fmt_ctx->streams[vid_idx];
    const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!dec) throw std::runtime_error("Decoder not found");

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, vs->codecpar);
    if (avcodec_open2(dec_ctx, dec, nullptr) < 0)
        throw std::runtime_error("Cannot open decoder");

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuva = av_frame_alloc();

    // Decode first frame
    bool got = false;
    while (av_read_frame(fmt_ctx, pkt) >= 0 && !got) {
        if (pkt->stream_index != vid_idx) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(dec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) { got = true; break; }
        }
        av_packet_unref(pkt);
    }
    if (!got) throw std::runtime_error("No frame decoded");

    // Convert to YUVA420P
    SwsContext* sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                     frame->width, frame->height, AV_PIX_FMT_YUVA420P,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    yuva->format = AV_PIX_FMT_YUVA420P;
    yuva->width  = frame->width;
    yuva->height = frame->height;
    av_frame_get_buffer(yuva, 32);
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
              yuva->data, yuva->linesize);

    // Clear alpha to opaque (255) so legend blends visibly later if used as overlay
    for (int y = 0; y < yuva->height; ++y)
        memset(yuva->data[3] + y * yuva->linesize[3], 255, yuva->width);

    // ---- draw legend ----
    std::vector<avs_analyzer::YUVAColor> cols = {
        {155, 48, 170, 120},
        {152, 165, 83, 220},
        { 107, 133, 62,	220},
      {199, 55, 145, 220},
{91, 173, 74},
{118, 69, 187},
{146, 136, 161, 220},
    };
    std::vector<std::u8string> labels = {
        u8"玩手机",
        u8"睡岗",
        u8"离岗",
      u8"闯入",
      u8"逗留",
      u8"攀爬",
      u8"未知",
    };

    // Load font and draw legend using FontManager
    avs_analyzer::FontManager fm;
    if (!fm.load_font(font_path, 32)) throw std::runtime_error("Failed to load font in test: " + font_path);

    avs_analyzer::draw_legend_yuva420p(
        yuva,
        fm,
        {1640, 40},     // origin
        32,            // font px
        {28, 18},      // handle size
        20,            // row spacing
        cols,
        labels);

    // ---- convert back to YUV420p for saving ----
    AVFrame* yuv420 = av_frame_alloc();
    SwsContext* sws_back = sws_getContext(yuva->width, yuva->height, AV_PIX_FMT_YUVA420P,
                                          yuva->width, yuva->height, AV_PIX_FMT_YUV420P,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    yuv420->format = AV_PIX_FMT_YUV420P;
    yuv420->width  = yuva->width;
    yuv420->height = yuva->height;
    av_frame_get_buffer(yuv420, 32);
    sws_scale(sws_back, yuva->data, yuva->linesize, 0, yuva->height,
              yuv420->data, yuv420->linesize);

    // ---- write to file ----
    {
        std::ofstream out(out_yuv_path, std::ios::binary);
        if (!out) throw std::runtime_error("Cannot open output file");

        for (int i = 0; i < 3; ++i) {
            int h = (i == 0) ? yuv420->height : (yuv420->height + 1) / 2;
            int w = (i == 0) ? yuv420->width  : (yuv420->width  + 1) / 2;
            for (int y = 0; y < h; ++y)
                out.write((char*)yuv420->data[i] + y * yuv420->linesize[i], w);
        }
    }

    std::cout << "✅ Legend drawn and saved to " << out_yuv_path
              << " (" << yuv420->width << "x" << yuv420->height << ")\n";

    // cleanup
    sws_freeContext(sws);
    sws_freeContext(sws_back);
    av_frame_free(&frame);
    av_frame_free(&yuva);
    av_frame_free(&yuv420);
    av_packet_free(&pkt);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cout << "Usage: " << argv[0] << " input.mp4 font.ttf output.yuv\n";
        return 0;
    }
    try {
        test_draw_legend_on_first_frame(argv[1], argv[2], argv[3]);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
