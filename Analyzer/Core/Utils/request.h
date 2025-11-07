#ifndef ANALYZER_REQUEST_H
#define ANALYZER_REQUEST_H

#include <string>
namespace avs_analyzer {
class Request {
 public:
  static bool get(const char* url, std::string& response);
  static bool post(const char* url, const char* data, std::string& response);
};
}  // namespace avs_analyzer
#endif  // ANALYZER_REQUEST_H