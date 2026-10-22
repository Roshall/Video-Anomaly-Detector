//
// Created by lg on 10/2/25.
//

#ifndef ANALYZER_TEST_HELPER_H
#define ANALYZER_TEST_HELPER_H
#include <functional>
#include <iostream>
#include <thread>  // For std::this_thread::sleep_for


extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include "streamer.h"

using avs_analyzer::Streamer;

int runner(Streamer&, const std::function<AVFrame*(AVFrame*, int&)>&,
           bool free_frame = true);
#endif  // ANALYZER_TEST_HELPER_H
