#include "Worker.h"

#include "Algorithm.h"
#include "Analyzer.h"
#include "AvPullStream.h"
#include "AvPushStream.h"
#include "Control.h"
#include "Frame.h"
#include "GenerateAlarmVideo.h"
#include "Scheduler.h"
#include "Utils/Common.h"
#include "Utils/Log.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>

#include "libswscale/swscale.h"
}

namespace avs_analyzer {

Worker::Worker(Scheduler* scheduler, std::unique_ptr<Monitor> monitor)
    : monitor(std::move(monitor)),
      scheduler(scheduler) {
  monitor->start_timestamp = getCurTimestamp();

  LOGI("");
}

Worker::~Worker() {
  LOGI("code=%s,streamUrl=%s finished.", monitor->code.data(),
       monitor->stream_url.data());

  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  state_ = false;  // 将执行状态设置为false
  threads_.clear();
  ClearAlarmVideoFrameQ();

  // unique_ptr members will be automatically cleaned up
}
bool Worker::start(std::string& msg) {
  this->in_stream = std::make_unique<AvPullStream>(this);
  if (this->in_stream->Connect()) {
    if (monitor->push_stream) {
      this->out_Stream = std::make_unique<AvPushStream>(this);
      if (this->out_Stream->Connect()) {
        // success
      } else {
        msg = "pull stream connect success, push stream connect error";
        return false;
      }
    } else {
      // success
    }
  } else {
    msg = "pull stream connect error";
    return false;
  }

  int videoBgrSize =
      monitor->video_height * monitor->video_width * monitor->video_channel;
  video_frame_pool = std::make_unique<FramePool>(videoBgrSize);
  analyzer = std::make_unique<Analyzer>(scheduler, monitor.get());

  state_ = true;  // 将执行状态设置为true
  threads_.reserve(4);
  threads_.push_back(std::make_unique<std::thread>(AvPullStream::ReadThread, this->in_stream.get()));

  threads_.push_back(std::make_unique<std::thread>(Worker::decodeVideoThread, this));

  threads_.push_back(std::make_unique<std::thread>(Worker::generateAlarmThread, this));

  if (monitor->push_stream) {
    if (monitor->video_index > -1) {
      threads_.push_back(std::make_unique<std::thread>(Worker::generateAlarmThread, this));
    }
  }
  return true;
}

bool Worker::getState() const { return state_; }
void Worker::remove() {
  state_ = false;
  scheduler->RemoveWorker(monitor->code);
}
void Worker::generateAlarmThread(void* arg) {
  auto worker = static_cast<Worker*>(arg);
  worker->handleGenerateAlarm();
}
void Worker::handleGenerateAlarm() {
  int width = monitor->video_width;
  int height = monitor->video_height;
  //        int channels = mControl->videoChannel;

  Frame* videoFrame = nullptr;  // 未编码的视频帧（bgr格式）

  std::queue<Frame*> cacheV;
  int prefix_size =
      30;  // 75 = 25 * 3，事件发生前缓存3秒的数据，1张压缩图片100kb

  bool happening = false;  // 当前是否正在发生报警行为
  std::queue<Frame*> happenV;
  int total_size = 60;  // 报警总帧数最大长度

  int64_t last_alarm_timestamp = 0;  // 上一次报警的时间戳

  while (getState()) {
    if (GetAlarmVideoFrame(videoFrame)) {
      if (happening && cacheV.empty()) {  // 报警事件已经发生，正在进行中

        happenV.push(videoFrame);

        if (happenV.size() >= total_size) {
          last_alarm_timestamp = getCurTimestamp();

          // 产生报警视频
          auto alarm = std::make_unique<Alarm>(
              height, width, monitor->video_fps, last_alarm_timestamp,
              prefix_size, monitor->code.data());

          while (!happenV.empty()) {
            Frame* p = happenV.front();
            happenV.pop();
            alarm->frames.push_back(p);
          }

          scheduler->AddAlarm(std::move(alarm));
          happening = false;
        }

      } else {  // 暂未发生报警事件

        if (!cacheV.empty() && cacheV.size() >= prefix_size) {
          // 缓存帧超过容量上限 prefix_size
          Frame* head = cacheV.front();
          cacheV.pop();
          video_frame_pool->giveBack(head);
        }
        cacheV.push(videoFrame);

        if (videoFrame->happen && cacheV.size() >= prefix_size &&
            (getCurTimestamp() - last_alarm_timestamp) > monitor->minInterval) {
          // 满足报警开始条件
          happening = true;

          while (!cacheV.empty()) {
            Frame* p = cacheV.front();
            cacheV.pop();
            happenV.push(p);
          }
        }
      }

    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  while (!happenV.empty()) {
    Frame* p = happenV.front();
    happenV.pop();
    delete p;
  }
  while (!cacheV.empty()) {
    Frame* p = cacheV.front();
    cacheV.pop();
    delete p;
  }
}
void Worker::decodeVideoThread(void* arg) {
  auto worker = (Worker*)arg;
  worker->handleDecodeVideo();
}
void Worker::handleDecodeVideo() {
  int width = in_stream->video_codec_ctx->width;
  int height = in_stream->video_codec_ctx->height;

  AVPacket pkt;      // 未解码的视频帧
  int pktQSize = 0;  // 未解码视频帧队列当前长度

  AVFrame* frame_yuv420p = av_frame_alloc();  // pkt->解码->frame
  AVFrame* frame_bgr = av_frame_alloc();

  int frame_bgr_buff_size =
      av_image_get_buffer_size(AV_PIX_FMT_BGR24, width, height, 1);
  auto frame_bgr_buff = (uint8_t*)av_malloc(frame_bgr_buff_size);
  av_image_fill_arrays(frame_bgr->data, frame_bgr->linesize, frame_bgr_buff,
                       AV_PIX_FMT_BGR24, width, height, 1);

  SwsContext* sws_ctx_yuv420p2bgr = sws_getContext(
      width, height, in_stream->video_codec_ctx->pix_fmt,
      in_stream->video_codec_ctx->width, in_stream->video_codec_ctx->height,
      AV_PIX_FMT_BGR24, SWS_BICUBIC, nullptr, nullptr, nullptr);

  //        int fps = mControl->videoFps;

  // 算法检测参数start
  bool cur_is_check = false;       // 当前帧是否进行算法检测
  int continuity_check_count = 0;  // 当前连续进行算法检测的帧数
  int continuity_check_max_time =
      6000;  // 连续进行算法检测，允许最长的时间。单位毫秒
  int64_t continuity_check_start = getCurTime();  // 单位毫秒
  int64_t continuity_check_end = 0;
  // 算法检测参数end

  int64_t frameCount = 0;
  std::vector<Detection> happenDetects;

  while (
      getState()) {  // TODO: make the detection FPS configurable and print
                     // warning msg if processing FPS blow the configured one.
                     //        If this frame isn't a must-check, simply cache
                     //        it.
    if (in_stream->GetVideoPkt(pkt, pktQSize)) {
      if (monitor->video_index > -1) {
        auto ret = avcodec_send_packet(in_stream->video_codec_ctx, &pkt);
        if (ret == 0) {
          ret =
              avcodec_receive_frame(in_stream->video_codec_ctx, frame_yuv420p);

          if (ret == 0) {
            frameCount++;

            // frame（yuv420p） 转 frame_bgr
            sws_scale(sws_ctx_yuv420p2bgr, frame_yuv420p->data,
                      frame_yuv420p->linesize, 0, height, frame_bgr->data,
                      frame_bgr->linesize);

            cv::Mat image(monitor->video_height, monitor->video_width, CV_8UC3,
                          frame_bgr->data[0]);

            bool happen = false;
            float happenScore = 0.0;

            if (pktQSize <= 1) {  // detect when the queue is fully consumed, to
                                  // prevent if from using up memory
              //                                int64_t t1 = getCurTimestamp();

              cur_is_check = analyzer->ProcessVideoFrame(
                  frameCount, image, happenDetects, happen, happenScore);

              //                                int64_t t2 = getCurTimestamp();
              //                                LOGI("t2 - t1 = %ld",(t2 - t1));

              if (cur_is_check) {
                continuity_check_count += 1;
              }
            }
            //                            else {
            //                                cur_is_check = false;
            //                            }

            continuity_check_end = getCurTime();
            if (continuity_check_end - continuity_check_start >
                continuity_check_max_time) {
              monitor->check_fps =
                  float(continuity_check_count) /
                  (float(continuity_check_end - continuity_check_start) / 1000);
              continuity_check_count = 0;
              continuity_check_start = getCurTime();
            }

            // 绘制start
            cv::polylines(image, monitor->ror_points,
                          !monitor->ror_points.empty(),
                          cv::Scalar(0, 0, 255), 4, 8);  // 绘制多边形
            if (!happenDetects.empty()) {
              for (auto& happenDetect : happenDetects) {
                if (happenDetect.happen) {
                  auto [x1, y1, x2, y2, class_score, _, class_name, _2] =
                      happenDetect;

                  std::stringstream class_score_ss;
                  class_score_ss << std::setprecision(1) << class_score;
                  std::string title = class_name + ":" + class_score_ss.str();

                  cv::rectangle(image, cv::Rect(x1, y1, (x2 - x1), (y2 - y1)),
                                cv::Scalar(0, 0, 255), 2, cv::LINE_8, 0);

                  // cv::rectangle(image, cv::Point(x1, y1 + 20), cv::Point(x2,
                  // y1), cv::Scalar(0, 255, 255), -1);

                  int h = y2 - y1;

                  cv::putText(image, title, cv::Point(x1, y1 + (h / 3)),
                              cv::FONT_HERSHEY_SIMPLEX, 2,
                              cv::Scalar(0, 0, 255), 2);
                }
              }
            }
            cv::putText(image, monitor->algorithm_code, cv::Point(20, 80),
                        cv::FONT_HERSHEY_COMPLEX, 2, cv::Scalar(0, 0, 255), 2);
            std::stringstream fps_stream;
            fps_stream << std::setprecision(4) << monitor->check_fps;
            std::string fps_title = "checkfps:" + fps_stream.str();
            cv::putText(image, fps_title, cv::Point(20, 140),
                        cv::FONT_HERSHEY_COMPLEX, 2, cv::Scalar(0, 0, 255), 1,
                        cv::LINE_AA);
            // 绘制end

            if (monitor->push_stream) {  // 需要推算法实时流
              int size = out_Stream->GetVideoFrameQSize();
              if (size > 3) {
                out_Stream->ClearVideoFrameQueue();
              }
              Frame* frame = video_frame_pool->gain();
              frame->SetBuf(frame_bgr->data[0], frame_bgr_buff_size);
              frame->happen = happen;
              frame->happen_score = happenScore;
              out_Stream->AddVideoFrame(frame);
            }

            // 添加合成报警视频帧start
            auto size = GetAlarmVideoFrameQSize();
            if (size > 3) {
              this->ClearAlarmVideoFrameQ();
            }

            Frame* frame = video_frame_pool->gain();
            frame->SetBuf(frame_bgr->data[0], frame_bgr_buff_size);
            frame->happen = happen;
            frame->happen_score = happenScore;
            this->AddAlarmVideoFrameQ(frame);

            // 添加合成报警视频帧end

          } else {
            LOGE("avcodec_receive_frame error : ret=%d", ret);
          }
        } else {
          LOGE("avcodec_send_packet error : ret=%d", ret);
        }
      }

      // 队列获取的pkt，必须释放!!!
      // av_free_packet(&pkt);//过时
      av_packet_unref(&pkt);
    } else {
      //                std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  av_frame_free(&frame_yuv420p);
  // av_frame_unref(frame_yuv420p);

  av_frame_free(&frame_bgr);
  // av_frame_unref(frame_bgr);

  av_free(frame_bgr_buff);

  sws_freeContext(sws_ctx_yuv420p2bgr);
}

void Worker::AddAlarmVideoFrameQ(Frame* frame) {
  frame_queue_mtx_.lock();
  alarm_frame_queue_.push(frame);
  frame_queue_mtx_.unlock();
}
size_t Worker::GetAlarmVideoFrameQSize() {
  frame_queue_mtx_.lock();
  auto size = alarm_frame_queue_.size();
  frame_queue_mtx_.unlock();
  return size;
}
bool Worker::GetAlarmVideoFrame(Frame*& frame) {
  bool res = false;
  if (frame_queue_mtx_.try_lock()) {
    if (!alarm_frame_queue_.empty()) {
      frame = alarm_frame_queue_.front();
      alarm_frame_queue_.pop();
      res = true;
    }
    frame_queue_mtx_.unlock();
  }
  return res;
}
void Worker::ClearAlarmVideoFrameQ() {
  frame_queue_mtx_.lock();
  while (!alarm_frame_queue_.empty()) {
    Frame* frame = alarm_frame_queue_.front();
    alarm_frame_queue_.pop();
    video_frame_pool->giveBack(frame);
  }
  frame_queue_mtx_.unlock();
}

}  // namespace avs_analyzer
