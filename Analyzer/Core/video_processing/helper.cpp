//
// Created by lg on 10/1/25.
//
#include "helper.h"
#include "Utils/log.h"

#include <iostream>
namespace avs_analyzer {
void log_error(const std::string& message, int error_code) {
  static char err_buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(error_code, err_buf, sizeof(err_buf));
  LOGE("[FFMPEG] {}: {} ({})", message, err_buf, error_code);
}
AVDictionaryPtr::AVDictionaryPtr(AVDictionaryPtr&& other) noexcept
    : p_(other.p_) { other.p_ = nullptr; }
AVDictionaryPtr& AVDictionaryPtr::operator=(AVDictionaryPtr&& other) noexcept {
  if (this != &other) {
    reset(nullptr);
    p_ = other.p_;
    other.p_ = nullptr;
  }
  return *this;
}
void AVDictionaryPtr::reset(AVDictionary* np) noexcept {
  if (p_ != np) {
    av_dict_free(&p_);
    p_ = np;
  }
}
AVDictionary* AVDictionaryPtr::release() noexcept {
  AVDictionary* tmp = p_;
  p_ = nullptr;
  return tmp;
}
int AVDictionaryPtr::set(const char* key, const char* val, int flags) {
  AVDictionary* raw = p_;
  int rc = av_dict_set(&raw, key, val, flags);
  // If av_dict_set changed the pointer, adopt the new one
  if (raw != p_) reset(raw);
  return rc;
}
}