#include "server_apis/api_common.h"

#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/http_struct.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>

#include "Utils/log.h"
#include "scheduler.h"
#include "config.h"

using namespace avs_analyzer;

namespace {
struct MultipartPart {
  std::string name;
  std::string filename;
  std::string content_type;
  std::vector<uint8_t> data;
};

// Parse multipart form data and extract parts
bool parse_multipart_data(const std::string& boundary,
                          const std::string& body,
                          std::vector<MultipartPart>& parts) {
  const std::string delimiter = "--" + boundary;
  const std::string end_delimiter = "--" + boundary + "--";
  
  size_t pos = 0;
  size_t start = 0;

  // Find first boundary
  start = body.find(delimiter, pos);
  if (start == std::string::npos) {
    LOGE("No boundary found in multipart data");
    return false;
  }

  pos = start + delimiter.length();

  while (pos < body.length()) {
    // Find next boundary
    size_t next_boundary = body.find("\r\n" + delimiter, pos);
    if (next_boundary == std::string::npos) {
      next_boundary = body.find(end_delimiter, pos);
      if (next_boundary == std::string::npos) {
        break;
      }
    }

    // Extract part between current position and next boundary
    std::string part = body.substr(pos, next_boundary - pos);
    
    // Remove leading CRLF
    if (part.substr(0, 2) == "\r\n") {
      part = part.substr(2);
    }

    // Parse headers
    size_t header_end = part.find("\r\n\r\n");
    if (header_end != std::string::npos) {
      std::string headers_str = part.substr(0, header_end);
      std::string body_str = part.substr(header_end + 4);
      
      // Remove trailing CRLF from body
      if (body_str.length() >= 2 && 
          body_str.substr(body_str.length() - 2) == "\r\n") {
        body_str = body_str.substr(0, body_str.length() - 2);
      }

      MultipartPart mp;

      // Parse Content-Disposition header
      const std::string cd_prefix = "Content-Disposition: form-data;";
      size_t cd_pos = headers_str.find(cd_prefix);
      if (cd_pos != std::string::npos) {
        cd_pos += cd_prefix.length();
        
        // Extract name
        size_t name_start = headers_str.find("name=\"", cd_pos);
        if (name_start != std::string::npos) {
          name_start += 6;
          size_t name_end = headers_str.find("\"", name_start);
          if (name_end != std::string::npos) {
            mp.name = headers_str.substr(name_start, name_end - name_start);
          }
        }

        // Extract filename
        size_t fn_start = headers_str.find("filename=\"", cd_pos);
        if (fn_start != std::string::npos) {
          fn_start += 10;
          size_t fn_end = headers_str.find("\"", fn_start);
          if (fn_end != std::string::npos) {
            mp.filename = headers_str.substr(fn_start, fn_end - fn_start);
          }
        }
      }

      // Parse Content-Type header
      const std::string ct_prefix = "Content-Type: ";
      size_t ct_pos = headers_str.find(ct_prefix);
      if (ct_pos != std::string::npos) {
        ct_pos += ct_prefix.length();
        size_t ct_end = headers_str.find("\r\n", ct_pos);
        if (ct_end == std::string::npos) {
          ct_end = headers_str.find("\n", ct_pos);
        }
        if (ct_end != std::string::npos) {
          mp.content_type = headers_str.substr(ct_pos, ct_end - ct_pos);
        }
      }

      // Store body data
      mp.data.assign(body_str.begin(), body_str.end());

      if (!mp.name.empty() || !mp.filename.empty()) {
        parts.push_back(mp);
      }
    }

    // Move to next part
    pos = next_boundary + delimiter.length();
    if (body.substr(next_boundary, 2) == "\r\n") {
      // Regular boundary follows CRLF
    } else if (body.substr(next_boundary, end_delimiter.length()) == end_delimiter) {
      // End boundary reached
      break;
    }
  }

  return !parts.empty();
}

std::string extract_boundary(const std::string& content_type) {
  const std::string boundary_prefix = "boundary=";
  size_t pos = content_type.find(boundary_prefix);
  if (pos == std::string::npos) {
    return "";
  }
  pos += boundary_prefix.length();
  
  // Remove quotes if present
  if (content_type[pos] == '"') {
    pos++;
    size_t end = content_type.find("\"", pos);
    if (end != std::string::npos) {
      return content_type.substr(pos, end - pos);
    }
  }
  
  size_t end = content_type.find(";", pos);
  if (end == std::string::npos) {
    return content_type.substr(pos);
  }
  return content_type.substr(pos, end - pos);
}
}  // namespace

void ApiFileUpload(evhttp_request* req, void* arg) {
  auto* scheduler = static_cast<avs_analyzer::Scheduler*>(arg);
  if (!scheduler) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"error":"scheduler not available"})");
    return;
  }

  // Check request method
  if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
    send_json(req, HTTP_BADMETHOD, R"({"error":"only POST method is supported"})");
    return;
  }

  const std::string offline_dir = scheduler->GetConfig().offline_video_dir;
  if (offline_dir.empty()) {
    send_json(req, HTTP_SERVUNAVAIL, R"({"error":"offline_video_dir is not configured"})");
    return;
  }

  // Create directory if it doesn't exist
  std::error_code ec;
  std::filesystem::create_directories(offline_dir, ec);
  if (ec) {
    auto err_msg = std::format(R"({{"error":"failed to create directory: {}}})", 
                               escape_json(ec.message()));
    send_json(req, HTTP_SERVUNAVAIL, err_msg);
    return;
  }

  // Get Content-Type header
  const char* content_type_header = 
      evhttp_find_header(evhttp_request_get_input_headers(req), "Content-Type");
  if (!content_type_header) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"Content-Type header is missing"})");
    return;
  }

  std::string content_type(content_type_header);
  
  // Check if it's multipart/form-data
  if (content_type.find("multipart/form-data") == std::string::npos) {
    send_json(req, HTTP_BADREQUEST, 
              R"({"error":"Content-Type must be multipart/form-data"})");
    return;
  }

  // Extract boundary
  std::string boundary = extract_boundary(content_type);
  if (boundary.empty()) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"boundary not found in Content-Type"})");
    return;
  }

  // Read request body
  size_t body_size = evbuffer_get_length(req->input_buffer);
  if (body_size == 0) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"request body is empty"})");
    return;
  }

  const char* body_ptr = reinterpret_cast<const char*>(
      evbuffer_pullup(req->input_buffer, -1));
  std::string body(body_ptr, body_size);

  // Parse multipart data
  std::vector<MultipartPart> parts;
  if (!parse_multipart_data(boundary, body, parts)) {
    send_json(req, HTTP_BADREQUEST, R"({"error":"failed to parse multipart data"})");
    return;
  }

  // Save uploaded files
  nlohmann::json result;
  nlohmann::json uploaded_files = nlohmann::json::array();
  std::string error_msg;
  size_t size{0};

  for (const auto& part : parts) {
    if (!part.filename.empty() && !part.data.empty()) {
      // Sanitize filename (remove path components)
      std::string safe_filename = std::filesystem::path(part.filename).filename().string();
      if (safe_filename.empty()) {
        safe_filename = part.name;
      }
      if (safe_filename.empty()) {
        safe_filename = "upload";
      }

      // Create full path
      std::filesystem::path file_path(offline_dir);
      file_path /= safe_filename;

      // Write file
      std::ofstream output_file(file_path, std::ios::binary);
      if (!output_file) {
        error_msg = std::format("failed to create file: {}", safe_filename);
        LOGE("{}", error_msg);
        continue;
      }

      output_file.write(reinterpret_cast<const char*>(part.data.data()), 
                       part.data.size());
      output_file.close();

      if (!output_file) {
        error_msg = std::format("failed to write file: {}", safe_filename);
        LOGE("{}", error_msg);
        continue;
      }

      nlohmann::json file_info;
      file_info["name"] = safe_filename;
      file_info["size"] = part.data.size();
      file_info["path"] = file_path.string();
      uploaded_files.push_back(file_info);

      LOGI("File uploaded successfully: {} ({} bytes) at {}", safe_filename, part.data.size(), file_path.string());
    }
  }
  

  result["success"] = error_msg.empty() && !uploaded_files.empty();
  result["files"] = uploaded_files;
  if (!error_msg.empty()) {
    result["errorMsg"] = error_msg;
  }

  send_json(req, HTTP_OK, result.dump());
}
