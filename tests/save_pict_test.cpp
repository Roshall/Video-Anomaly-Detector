//
// Created by lg on 10/10/25.
//
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

#include "jpeg_encoder.h"  // Assumes JPEGEncoder class is in a separate header file

void test_jpeg_encoder(const std::string& video1_path, const std::string& video2_path) {
  AVFormatContext* fmt_ctx = nullptr;
  AVCodecContext* codec_ctx = nullptr;
  AVFrame* frame = nullptr;
  AVPacket* pkt = nullptr;
  int video_stream_idx = -1;

  // Initialize JPEGEncoder with a default size (will be adjusted if needed)
  avs_analyzer::JPEGEncoder encoder(1280, 720);

  // Process two videos
  for (const auto& video_path : {video1_path, video2_path}) {
    // Open video file
    if (avformat_open_input(&fmt_ctx, video_path.c_str(), nullptr, nullptr) < 0) {
      throw std::runtime_error("Failed to open video file: " + video_path);
    }

    // Find stream info
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("Failed to find stream info for: " + video_path);
    }

    // Find video stream
    video_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; ++i) {
      if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        video_stream_idx = i;
        break;
      }
    }
    if (video_stream_idx == -1) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("No video stream found in: " + video_path);
    }

    // Find decoder
    auto codec = avcodec_find_decoder(fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
    if (!codec) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("Decoder not found for: " + video_path);
    }

    // Allocate codec context
    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("Failed to allocate codec context");
    }

    // Copy codec parameters
    if (avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[video_stream_idx]->codecpar) < 0) {
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("Failed to copy codec parameters");
    }

    // Open codec
    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("Failed to open codec");
    }

    // Allocate frame and packet
    frame = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!frame || !pkt) {
      av_frame_free(&frame);
      av_packet_free(&pkt);
      avcodec_free_context(&codec_ctx);
      avformat_close_input(&fmt_ctx);
      throw std::runtime_error("Failed to allocate frame or packet");
    }

    // Read and save first 10 frames
    int frame_count = 0;
    while (av_read_frame(fmt_ctx, pkt) >= 0 && frame_count < 10) {
      if (pkt->stream_index == video_stream_idx) {
        // Send packet to decoder
        if (avcodec_send_packet(codec_ctx, pkt) < 0) {
          av_packet_unref(pkt);
          continue;
        }

        // Receive decoded frame
        while (avcodec_receive_frame(codec_ctx, frame) == 0) {
          if (frame->format == AV_PIX_FMT_YUV420P) {
            // Generate output filename
            std::string output_file = video_path.substr(video_path.find_last_of("/\\") + 1);
            output_file = output_file.substr(0, output_file.find_last_of(".")) + "_frame" +
                          std::to_string(frame_count + 1) + ".jpg";

            // Save frame as JPEG
            if (encoder.saveFrame(frame, output_file) < 0) {
              fprintf(stderr, "Failed to save frame %d for %s\n", frame_count + 1, video_path.c_str());
            } else {
              printf("Saved %s\n", output_file.c_str());
            }
            frame_count++;
            if (frame_count >= 10) break;
          }
        }
      }
      av_packet_unref(pkt);
    }

    // Flush decoder
    avcodec_send_packet(codec_ctx, nullptr);
    while (frame_count < 10 && avcodec_receive_frame(codec_ctx, frame) == 0) {
      if (frame->format == AV_PIX_FMT_YUV420P) {
        std::string output_file = video_path.substr(video_path.find_last_of("/\\") + 1);
        output_file = output_file.substr(0, output_file.find_last_of(".")) + "_frame" +
                      std::to_string(frame_count + 1) + ".jpg";
        if (encoder.saveFrame(frame, output_file) < 0) {
          fprintf(stderr, "Failed to save frame %d for %s\n", frame_count + 1, video_path.c_str());
        } else {
          printf("Saved %s\n", output_file.c_str());
        }
        frame_count++;
      }
    }

    // Cleanup for this video
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&fmt_ctx);
  }
}

int main() {
  try {
    test_jpeg_encoder("/home/lg/CXXProjects/BXC_VideoAnalyzer_v3.51/data/720p_fight.mp4", "/home/lg/CXXProjects/BXC_VideoAnalyzer_v3.51/data/720p_fire.mp4");
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}