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

static std::u8string repeated_measure_label(int count) {
    const std::u8string unit = u8"测";
    std::u8string out;
    out.reserve(static_cast<size_t>(std::max(0, count)) * unit.size());
    for (int i = 0; i < count; ++i) {
        out += unit;
    }
    return out;
}

static void write_yuv420p_frame(std::ofstream& out, const AVFrame* frame) {
    for (int i = 0; i < 3; ++i) {
        const int h = (i == 0) ? frame->height : (frame->height + 1) / 2;
        const int w = (i == 0) ? frame->width : (frame->width + 1) / 2;
        for (int y = 0; y < h; ++y) {
            out.write(reinterpret_cast<const char*>(frame->data[i]) + y * frame->linesize[i], w);
        }
    }
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

    // Prepare shared video context for all configuration cases
    VideoContext vctx{};
    vctx.width = vs->codecpar->width;
    vctx.height = vs->codecpar->height;
    vctx.format = AV_PIX_FMT_YUV420P; // we will feed YUV420P
    vctx.video_index = vid_idx;
    vctx.time_base = vs->time_base;
    vctx.sample_aspect_ratio = vs->sample_aspect_ratio;

    // Prepare to decode frames: we'll collect first 3 decoded frames
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* yuv420 = av_frame_alloc();
    int decoded_count = 0;

    std::vector<AVFrame*> decoded_frames;

    while (decoded_count < 3 && av_read_frame(fmt_ctx, pkt) >= 0) {
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
                if (decoded_count >= 3) break;
            }
        }
        av_packet_unref(pkt);
    }
    if (decoded_frames.size() < 3) {
        std::cerr << "Insufficient frames decoded: got " << decoded_frames.size() << "\n";
        return 1;
    }

    // Open output file to save 3 YUV frames (one per configuration case)
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

    struct CaseConfig {
        const char* name;
        int labels;
        int label_len;
    };

    const std::vector<CaseConfig> cases = {
        {"max", DynamicAnnotator::LegendConfig::kMaxPreallocLabels,
                DynamicAnnotator::LegendConfig::kMaxPreallocLabelLen},
        {"min", DynamicAnnotator::LegendConfig::kMinPreallocLabels,
                DynamicAnnotator::LegendConfig::kMinPreallocLabelLen},
        {"half", DynamicAnnotator::LegendConfig::kMaxPreallocLabels / 2,
                 DynamicAnnotator::LegendConfig::kMaxPreallocLabelLen / 2}
    };

    const int fw = vctx.width;
    const int fh = vctx.height;

    for (size_t idx = 0; idx < cases.size(); ++idx) {
        const auto& c = cases[idx];
        DynamicAnnotator annotator;
        DynamicAnnotator::LegendConfig lconf;
        lconf.font_path = font_path;
        lconf.font_size = 24;
        lconf.prealloc_max_labels = c.labels;
        lconf.prealloc_max_label_len = c.label_len;
        lconf.SetLocation("top-right");

        if (annotator.init(vctx, lconf) < 0) {
            std::cerr << "Annotator init failed for case: " << c.name << "\n";
            return 1;
        }

        std::vector<std::pair<Task, std::u8string>> legend;
        legend.reserve(static_cast<size_t>(c.labels));
        for (int i = 0; i < c.labels; ++i) {
            legend.emplace_back(all_tasks[static_cast<size_t>(i % all_tasks.size())],
                                repeated_measure_label(c.label_len));
        }
        annotator.update_legend(legend);

        std::vector<DynamicAnnotator::ROIEntry> rois;
        std::vector<DynamicAnnotator::BBoxEntry> bboxes;
        rois.reserve(static_cast<size_t>(c.labels));
        bboxes.reserve(static_cast<size_t>(c.labels));
        for (int i = 0; i < c.labels; ++i) {
            const Task task = all_tasks[static_cast<size_t>(i % all_tasks.size())];
            rois.emplace_back(task, random_quad(fw, fh));
            bboxes.emplace_back(task, random_bbox(fw, fh));
        }

        AVFrame* input_clone = av_frame_clone(decoded_frames[idx]);
        if (!input_clone) {
            std::cerr << "Failed to clone input frame for case: " << c.name << "\n";
            return 1;
        }

        AVFramePtr result = annotator.draw_elements(input_clone, rois, bboxes);
        av_frame_free(&input_clone);
        if (!result) {
            std::cerr << "draw_elements failed for case: " << c.name << "\n";
            return 1;
        }
        if (result->format != AV_PIX_FMT_YUV420P) {
            std::cerr << "Unexpected output pixel format for case: " << c.name << std::endl;
            return 1;
        }

        write_yuv420p_frame(out, result.get());
    }

    // cleanup
    for (auto f : decoded_frames) av_frame_free(&f);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&yuv420);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    out.close();

    std::cout << "✅ Wrote 3 annotated frames (max/min/half prealloc config) to " << out_path << "\n";
    return 0;
}
