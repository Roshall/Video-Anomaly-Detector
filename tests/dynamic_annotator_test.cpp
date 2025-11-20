// dynamic_annotator_test.cpp
// Visual test for DynamicAnnotator: decode four frames and write annotated images to a YUV file

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <random>

#include "dynamic_annotator.h"
#include "Utils/enum_utils.h"
#include "task.h"
#include <opencv2/opencv.hpp>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
}

using namespace avs_analyzer;

static std::mt19937 rng(12345);

static cv::Rect random_bbox(int frame_w, int frame_h, int minw = 20, int minh = 20, int maxw = 300, int maxh = 300) {
    std::uniform_int_distribution<int> wx(0, frame_w - 1);
    std::uniform_int_distribution<int> wy(0, frame_h - 1);
    int x = wx(rng);
    int y = wy(rng);
    std::uniform_int_distribution<int> dw(minw, std::max(minw, std::min(maxw, frame_w - x)));
    std::uniform_int_distribution<int> dh(minh, std::max(minh, std::min(maxh, frame_h - y)));
    int w = dw(rng);
    int h = dh(rng);
    // clamp
    if (x + w > frame_w) w = frame_w - x;
    if (y + h > frame_h) h = frame_h - y;
    return cv::Rect(x, y, w, h);
}

static std::vector<cv::Point> random_quad(int frame_w, int frame_h) {
    // Create a roughly rectangular quadrilateral
    std::uniform_int_distribution<int> wx(0, frame_w - 1);
    std::uniform_int_distribution<int> wy(0, frame_h - 1);
    int x0 = wx(rng);
    int y0 = wy(rng);
    std::uniform_int_distribution<int> rw(30, std::max(30, frame_w / 4));
    std::uniform_int_distribution<int> rh(30, std::max(30, frame_h / 4));
    int w = std::min(rw(rng), frame_w - x0);
    int h = std::min(rh(rng), frame_h - y0);
    std::vector<cv::Point> q(4);
    q[0] = cv::Point(x0, y0);
    q[1] = cv::Point(x0 + w, y0);
    q[2] = cv::Point(x0 + w, y0 + h);
    q[3] = cv::Point(x0, y0 + h);
    return q;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cout << "Usage: " << argv[0] << " input.mp4 font.ttf out.yuv\n";
        return 0;
    }
    const std::string input_path = argv[1];
    const std::string font_path = argv[2];
    const std::string out_path = argv[3];

    av_log_set_level(AV_LOG_ERROR);

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_open_input(&fmt_ctx, input_path.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Cannot open input video: " << input_path << "\n";
        return 1;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "Cannot find stream info\n";
        return 1;
    }
    int vid_idx = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vid_idx < 0) {
        std::cerr << "No video stream found\n";
        return 1;
    }

    AVStream* vs = fmt_ctx->streams[vid_idx];
    const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
    if (!dec) {
        std::cerr << "Decoder not found\n";
        return 1;
    }

    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(dec_ctx, vs->codecpar);
    if (avcodec_open2(dec_ctx, dec, nullptr) < 0) {
        std::cerr << "Cannot open decoder\n";
        return 1;
    }

    // Prepare dynamic annotator
    DynamicAnnotator annotator;
    VideoContext vctx{};
    vctx.width = vs->codecpar->width;
    vctx.height = vs->codecpar->height;
    vctx.format = AV_PIX_FMT_YUV420P; // we will feed YUV420P
    vctx.video_index = vid_idx;
    vctx.time_base = vs->time_base;
    vctx.sample_aspect_ratio = vs->sample_aspect_ratio;

    DynamicAnnotator::LegendConfig lconf;
    lconf.font_path = font_path;
    lconf.font_size = 18;
    // init with legend config; this will pre-allocate legend overlay using full Task enum
    if (annotator.init(vctx, lconf) < 0) {
        std::cerr << "Annotator init failed\n";
        return 1;
    }

    // Prepare to decode frames: we'll collect first 4 decoded frames
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuv420 = av_frame_alloc();
    int decoded_count = 0;

    std::vector<AVFrame*> decoded_frames;

    while (decoded_count < 4 && av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index != vid_idx) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(dec_ctx, pkt) == 0) {
            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                // Convert to YUV420P
                SwsContext* sws = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                                 frame->width, frame->height, AV_PIX_FMT_YUV420P,
                                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
                yuv420->format = AV_PIX_FMT_YUV420P;
                yuv420->width = frame->width;
                yuv420->height = frame->height;
                av_frame_get_buffer(yuv420, 32);
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, yuv420->data, yuv420->linesize);
                sws_freeContext(sws);
                // Clone the YUV frame (create a new frame to store a copy)
                AVFrame* copy = av_frame_clone(yuv420);
                decoded_frames.push_back(copy);
                decoded_count++;
                av_frame_unref(yuv420);
                if (decoded_count >= 4) break;
            }
        }
        av_packet_unref(pkt);
    }
    if (decoded_frames.size() < 4) {
        std::cerr << "Insufficient frames decoded: got " << decoded_frames.size() << "\n";
        return 1;
    }

    // Open output file to save 4 YUV frames
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::cerr << "Cannot open output file" << out_path << "\n";
        return 1;
    }

    // Choose tasks
    std::vector<Task> all_tasks;
    for (const auto t : get_enum_range<Task>()) all_tasks.push_back(t);
    // assert at least 6 tasks
    if (all_tasks.size() < 6) {
        std::cerr << "Not enough Task enums detected" << std::endl;
        return 1;
    }

    // Frame 1: first 4 tasks -> update legend
    std::vector<Task> tasks4{all_tasks.begin(), all_tasks.begin() + 4};
    annotator.update_legend(tasks4);

    // Generate ROIs & bboxes
    const int fw = vctx.width;
    const int fh = vctx.height;

    // Frame 1: 4 ROIs (one per task), and 8 bboxes (2 per task)
    {
        std::vector<DynamicAnnotator::ROIEntry> rois;
        std::vector<DynamicAnnotator::BBoxEntry> bboxes;
        for (int i = 0; i < 4; ++i) {
            rois.emplace_back(tasks4[i], random_quad(fw, fh));
            // 2 bboxes per task
            for (int j = 0; j < 2; ++j) {
                bboxes.emplace_back(tasks4[i], random_bbox(fw, fh));
            }
        }
        AVFramePtr result = annotator.draw_elements(decoded_frames[0], rois, bboxes);
        if (!result) {
            std::cerr << "draw_elements failed for frame 1\n";
            return 1;
        }
        // write out result YUV420P frames
        // ensure we have YUV420P
        if (result->format != AV_PIX_FMT_YUV420P) {
            std::cerr << "Unexpected output pixel format for frame 1" << std::endl;
            return 1;
        }
        // Write planes Y U V
        for (int i = 0; i < 3; ++i) {
            int h = (i == 0) ? result->height : (result->height + 1) / 2;
            int w = (i == 0) ? result->width : (result->width + 1) / 2;
            for (int y = 0; y < h; ++y)
                out.write((char*)result->data[i] + y * result->linesize[i], w);
        }
    }

    // Frame 2: 3 ROIs (take first 3 of tasks4) and 3 bboxes (one per task)
    {
        std::vector<Task> tasks3{tasks4.begin(), tasks4.begin() + 3};
        std::vector<DynamicAnnotator::ROIEntry> rois;
        std::vector<DynamicAnnotator::BBoxEntry> bboxes;
        for (int i = 0; i < 3; ++i) {
            rois.emplace_back(tasks3[i], random_quad(fw, fh));
            bboxes.emplace_back(tasks3[i], random_bbox(fw, fh));
        }
        AVFramePtr result = annotator.draw_elements(decoded_frames[1], rois, bboxes);
        if (!result) {
            std::cerr << "draw_elements failed for frame 2\n";
            return 1;
        }
        if (result->format != AV_PIX_FMT_YUV420P) {
            std::cerr << "Unexpected output pixel format for frame 2" << std::endl;
            return 1;
        }
        for (int i = 0; i < 3; ++i) {
            int h = (i == 0) ? result->height : (result->height + 1) / 2;
            int w = (i == 0) ? result->width : (result->width + 1) / 2;
            for (int y = 0; y < h; ++y)
                out.write((char*)result->data[i] + y * result->linesize[i], w);
        }
    }

    // Frame 3: update legend to have all 6 tasks, draw 6 rois and 6 bboxes
    {
        std::vector<Task> tasks6{all_tasks.begin(), all_tasks.begin() + 6};
        annotator.update_legend(tasks6);
        std::vector<DynamicAnnotator::ROIEntry> rois;
        std::vector<DynamicAnnotator::BBoxEntry> bboxes;
        for (int i = 0; i < 6; ++i) {
            rois.emplace_back(tasks6[i], random_quad(fw, fh));
            bboxes.emplace_back(tasks6[i], random_bbox(fw, fh));
        }
        AVFramePtr result = annotator.draw_elements(decoded_frames[2], rois, bboxes);
        if (!result) {
            std::cerr << "draw_elements failed for frame 3\n";
            return 1;
        }
        if (result->format != AV_PIX_FMT_YUV420P) {
            std::cerr << "Unexpected output pixel format for frame 3" << std::endl;
            return 1;
        }
        for (int i = 0; i < 3; ++i) {
            int h = (i == 0) ? result->height : (result->height + 1) / 2;
            int w = (i == 0) ? result->width : (result->width + 1) / 2;
            for (int y = 0; y < h; ++y)
                out.write((char*)result->data[i] + y * result->linesize[i], w);
        }
    }

    // Frame 4: reduce legend to 1 task
    {
        std::vector<Task> tasks1{all_tasks[0]};
        annotator.update_legend(tasks1);
        std::vector<DynamicAnnotator::ROIEntry> rois;
        std::vector<DynamicAnnotator::BBoxEntry> bboxes;
        rois.emplace_back(tasks1[0], random_quad(fw, fh));
        bboxes.emplace_back(tasks1[0], random_bbox(fw, fh));
        AVFramePtr result = annotator.draw_elements(decoded_frames[3], rois, bboxes);
        if (!result) {
            std::cerr << "draw_elements failed for frame 4\n";
            return 1;
        }
        if (result->format != AV_PIX_FMT_YUV420P) {
            std::cerr << "Unexpected output pixel format for frame 4" << std::endl;
            return 1;
        }
        for (int i = 0; i < 3; ++i) {
            int h = (i == 0) ? result->height : (result->height + 1) / 2;
            int w = (i == 0) ? result->width : (result->width + 1) / 2;
            for (int y = 0; y < h; ++y)
                out.write((char*)result->data[i] + y * result->linesize[i], w);
        }
    }

    // cleanup
    for (auto f : decoded_frames) av_frame_free(&f);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&yuv420);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    out.close();

    std::cout << "✅ Wrote 4 annotated frames to " << out_path << "\n";
    return 0;
}
