//
// Created by lg on 10/1/25.
//
#include "helper.h"

#include <iostream>
namespace avs_analyzer {
void log_error(const std::string& message, int error_code) {
  static char err_buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(error_code, err_buf, sizeof(err_buf));
  std::cerr << message << ": " << err_buf << " ( " << error_code << ")"
            << std::endl;
}
}