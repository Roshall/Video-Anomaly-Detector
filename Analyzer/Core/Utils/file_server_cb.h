//
// Created by lg on 11/6/25.
//

#ifndef ANALYZER_FILE_SERVER_H
#define ANALYZER_FILE_SERVER_H

#include <event2/buffer.h>
#include <event2/http.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <ctime>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

struct FileRouteOptions {
  std::string root_dir;          // e.g., "/var/www/files" (must be absolute)
  std::string route_prefix;      // e.g., "/video" - will be stripped from request path
  std::string default_file; // optional file to serve when path ends with '/'

  // Cache control
  uint32_t max_age_seconds = 3600; // Default 1 hour cache
  bool enable_cors = false;        // Enable CORS headers

  // Security
  bool follow_symlinks = false;    // Allow symlinks (dangerous!)
  bool enable_directory_listing = false; // TODO: implement directory listing
};

// Extended MIME type map
// Extended MIME type map using std::unordered_map
inline std::string mime_for_path(std::string_view path) {
  const auto dot = path.find_last_of('.');
  if (dot == std::string_view::npos) {
    return "application/octet-stream";
  }

  std::string ext(path.substr(dot + 1));
  std::ranges::transform(ext, ext.begin(),
                 [](unsigned char c){ return std::tolower(c); });

  // Static map for O(1) average-case lookup
  static const std::unordered_map<std::string, std::string> mime_map = {
    // Video formats
    {"ts", "video/mp2t"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mkv", "video/x-matroska"},
    {"avi", "video/x-msvideo"},
    {"mov", "video/quicktime"},
    {"m4v", "video/x-m4v"},

    // Streaming
    {"m3u8", "application/vnd.apple.mpegurl"},
    {"mpd", "application/dash+xml"},

    // Audio formats
    {"mp3", "audio/mpeg"},
    {"aac", "audio/aac"},
    {"ogg", "audio/ogg"},
    {"opus", "audio/opus"},
    {"flac", "audio/flac"},
    {"wav", "audio/wav"},
    {"m4a", "audio/mp4"},

    // Images
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"png", "image/png"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"svg", "image/svg+xml"},
    {"ico", "image/x-icon"},

    // Text
    {"txt", "text/plain; charset=utf-8"},
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "application/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"xml", "application/xml; charset=utf-8"}
  };

  if (const auto it = mime_map.find(ext); it != mime_map.end()) {
    return it->second;
  }

  return "application/octet-stream";
}

// URL-decode (percent-decoding) using libevent helper
inline std::string url_decode(const char* s) {
  size_t err = 0;
  char* decoded = evhttp_uridecode(s, 0, &err);
  if (!decoded || err) {
    if (decoded) free(decoded);
    // Fallback: if there is no percent-encoding, treat the path as already decoded.
    // This avoids spuriously rejecting plain paths on some libevent builds.
    if (s && std::strchr(s, '%') == nullptr) {
      return std::string(s);
    }
    return {};
  }
  std::string out(decoded);
  free(decoded);
  return out;
}

// Secure path joining with realpath canonicalization
// Ensures the final path is under root_dir and handles all traversal attempts
inline bool safe_join_path(const std::string& root, const std::string& rel,
                           std::string* out) {
  // Quick rejection of obvious traversal attempts
  if (rel.find("..") != std::string::npos) {
    return false;
  }

  // Reject absolute paths in rel (we want relative to root)
  if (!rel.empty() && rel.front() == '/') {
    // Actually, after route prefix removal, we might have leading /
    // So we'll allow it but treat as relative to root
  }

  // Build candidate path
  std::string candidate = root;
  if (!candidate.empty() && candidate.back() != '/') {
    candidate += '/';
  }
  // Remove leading / from rel if present
  std::string clean_rel = rel;
  while (!clean_rel.empty() && clean_rel.front() == '/') {
    clean_rel.erase(0, 1);
  }
  candidate += clean_rel;

  // Canonicalize root directory
  std::array<char, PATH_MAX> resolved_root{};
  if (!realpath(root.c_str(), resolved_root.data())) {
    return false; // Root directory doesn't exist or not accessible
  }

  // For candidate, we need to handle the case where file doesn't exist yet
  // realpath() fails if the file doesn't exist, so we need to check parent
  std::array<char, PATH_MAX> resolved_candidate{};

  // Try to resolve the full path

  if (const char* result = realpath(candidate.c_str(), resolved_candidate.data());
      !result) {
    // File might not exist; try resolving parent directory
    const auto last_slash = candidate.find_last_of('/');
    if (last_slash == std::string::npos) {
      return false;
    }

    std::string parent = candidate.substr(0, last_slash);
    const std::string filename = candidate.substr(last_slash + 1);

    if (parent.empty()) parent = "/";

    std::array<char, PATH_MAX> resolved_parent{};
    if (!realpath(parent.c_str(), resolved_parent.data())) {
      return false; // Parent directory doesn't exist
    }

    // Reconstruct path
    std::string reconstructed {resolved_parent.data()};
    if (reconstructed.back() != '/') {
      reconstructed += '/';
    }
    reconstructed += filename;

    // Verify it's under root
    std::string root_str(resolved_root.data());
    if (root_str.back() != '/') root_str += '/';

    if (reconstructed.rfind(root_str, 0) != 0) {
      return false; // Path escapes root
    }

    *out = reconstructed;
    return true;
  }

  // File exists; verify it's under root
  std::string root_str(resolved_root.data());
  const std::string cand_str(resolved_candidate.data());

  if (root_str.back() != '/') root_str += '/';

  if (cand_str.rfind(root_str, 0) != 0) {
    return false; // Path escapes root directory
  }

  *out = cand_str;
  return true;
}

// Format HTTP date (RFC 7231)
inline std::string http_date(time_t t) {
  tm tm{};
  gmtime_r(&t, &tm);
  std::array<char, 64> buf{};
  strftime(buf.data(), buf.size(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
  return {buf.data()};
}

// Generate simple ETag from file metadata
inline std::string generate_etag(const struct stat& st) {
  std::ostringstream oss;
  oss << std::hex << st.st_ino << "-" << st.st_size << "-" << st.st_mtime;
  return "\"" + oss.str() + "\"";
}

// Parse Range header: bytes=start-end
// Returns {start, end_inclusive}. If header missing/invalid -> nullopt.
inline std::optional<std::pair<off_t, off_t>>
parse_range(const char* range_hdr, off_t file_size) {
  if (!range_hdr || file_size <= 0) return std::nullopt;

  const std::string_view sv(range_hdr);
  constexpr std::string_view prefix = "bytes=";

  if (sv.rfind(prefix, 0) != 0) return std::nullopt;

  auto spec = sv.substr(prefix.size());

  // Only handle single range (not multipart ranges like "0-100,200-300")
  if (const auto comma = spec.find(','); comma != std::string_view::npos) {
    return std::nullopt; // Reject multipart ranges
  }

  const auto dash = spec.find('-');
  if (dash == std::string_view::npos) return std::nullopt;

  const auto start_str = std::string(spec.substr(0, dash));
  const auto end_str   = std::string(spec.substr(dash + 1));

  try {
    if (!start_str.empty()) {
      off_t start = std::stoll(start_str);
      if (start < 0) return std::nullopt;

      if (!end_str.empty()) {
        // Form: start-end
        off_t end = std::stoll(end_str);
        if (end < start) return std::nullopt;

        // Clamp to file size
        start = std::min(start, file_size - 1);
        end = std::min(end, file_size - 1);

        return std::make_pair(start, end);
      }
      // Form: start- (to end of file)
      start = std::min(start, file_size - 1);
      return std::make_pair(start, file_size - 1);
    }
    // Form: -suffix_length (last N bytes)
    if (end_str.empty()) return std::nullopt;

    const off_t suffix = std::stoll(end_str);
    if (suffix <= 0) return std::nullopt;

    const off_t len = std::min(suffix, file_size);
    return std::make_pair(file_size - len, file_size - 1);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// Extract filename from path for Content-Disposition header
inline std::string extract_filename(const std::string& path) {
  const auto pos = path.find_last_of('/');
  if (pos == std::string::npos) return path;
  return path.substr(pos + 1);
}

// Parses an RFC 7231 HTTP date
inline std::optional<time_t> parse_http_date(const std::string& date_str) {
  tm tm = {};
  // strptime is POSIX.1-2001
  if (char* unparsed =
          strptime(date_str.c_str(), "%a, %d %b %Y %H:%M:%S GMT", &tm);
      unparsed == nullptr || *unparsed != '\0') {
    return std::nullopt; // Failed to parse
  }
  // timegm is a common extension (GNU, BSD) for converting GMT tm to time_t
  return timegm(&tm);
}

// Main file serving callback
inline void file_route_cb(evhttp_request* req, void* arg) {
  auto* opt = static_cast<FileRouteOptions*>(arg);
  if (!opt) {
    evhttp_send_error(req, HTTP_INTERNAL, "Server misconfigured");
    return;
  }

  // Method validation
  const auto method = evhttp_request_get_command(req);
  if (method != EVHTTP_REQ_GET && method != EVHTTP_REQ_HEAD) {
    auto* out = evhttp_request_get_output_headers(req);
    evhttp_add_header(out, "Allow", "GET, HEAD");
    evhttp_send_error(req, HTTP_BADMETHOD, "Method Not Allowed");
    return;
  }

  // Parse URI
  const evhttp_uri* uri = evhttp_request_get_evhttp_uri(req);
  if (!uri) {
    evhttp_send_error(req, HTTP_BADREQUEST, "Invalid URI");
    return;
  }

  const char* raw_path = evhttp_uri_get_path(uri);
  if (!raw_path) raw_path = "/";

  // URL decode
  std::string decoded_path = url_decode(raw_path);
  if (decoded_path.empty()) {
    evhttp_send_error(req, HTTP_BADREQUEST, "Invalid path encoding");
    return;
  }

  // Strip route prefix if configured
  if (!opt->route_prefix.empty()) {
    if (decoded_path.rfind(opt->route_prefix, 0) == 0) {
      decoded_path.erase(0, opt->route_prefix.size());
    } else {
      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
      return;
    }
  }

  // Ensure path starts with / for consistency
  if (decoded_path.empty() || decoded_path.front() != '/') {
    decoded_path = "/" + decoded_path;
  }

  // Handle default file for directory requests
  if (!opt->default_file.empty() && decoded_path.back() == '/') {
    decoded_path += opt->default_file;
  }

  // Securely join path with root
  std::string local_path;
  if (!safe_join_path(opt->root_dir, decoded_path, &local_path)) {
    evhttp_send_error(req, HTTP_BADREQUEST, "Invalid path");
    return;
  }

  // Open file with security flags
  int open_flags = O_RDONLY;
  // O_NOFOLLOW prevents opening the file if the final path component
  // is a symlink. Note that symlinks in the parent directories were
  // already resolved by realpath() in safe_join_path.
  if (!opt->follow_symlinks) {
    open_flags |= O_NOFOLLOW; // Don't follow symlinks
  }

  int fd = open(local_path.c_str(), open_flags);
  if (fd < 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      evhttp_send_error(req, HTTP_NOTFOUND, "Not Found");
    } else if (errno == EACCES) {
      evhttp_send_error(req, HTTP_BADMETHOD, "Forbidden");
    } else if (errno == ELOOP) {
      evhttp_send_error(req, HTTP_BADMETHOD, "Symlink not allowed");
    } else {
      evhttp_send_error(req, HTTP_INTERNAL, "Unable to open file");
    }
    return;
  }

  // Stat the file AFTER opening (prevents TOCTOU races)
  struct stat st{};
  if (fstat(fd, &st) != 0) {
    close(fd);
    evhttp_send_error(req, HTTP_INTERNAL, "Unable to stat file");
    return;
  }

  // Verify it's a regular file
  if (!S_ISREG(st.st_mode)) {
    close(fd);
    if (S_ISDIR(st.st_mode)) {
      evhttp_send_error(req, HTTP_BADMETHOD, "Directory listing not allowed");
    } else {
      evhttp_send_error(req, HTTP_BADMETHOD, "Not a regular file");
    }
    return;
  }

  off_t file_size = st.st_size;

  // Get headers
  auto* in_headers  = evhttp_request_get_input_headers(req);
  auto* out_headers = evhttp_request_get_output_headers(req);

  // Set MIME type
  std::string mime_type = mime_for_path(local_path);
  evhttp_add_header(out_headers, "Content-Type", mime_type.c_str());

  // Set caching headers
  evhttp_add_header(out_headers, "Cache-Control",
                    ("public, max-age=" + std::to_string(opt->max_age_seconds)).c_str());
  evhttp_add_header(out_headers, "Last-Modified", http_date(st.st_mtime).c_str());

  // Set ETag
  std::string etag = generate_etag(st);
  evhttp_add_header(out_headers, "ETag", etag.c_str());

  // CORS headers if enabled
  if (opt->enable_cors) {
    evhttp_add_header(out_headers, "Access-Control-Allow-Origin", "*");
    evhttp_add_header(out_headers, "Access-Control-Allow-Methods", "GET, HEAD");
    evhttp_add_header(out_headers, "Access-Control-Max-Age", "3600");
  }

  // Accept ranges for streaming
  evhttp_add_header(out_headers, "Accept-Ranges", "bytes");

  // Content-Disposition (inline for video/audio, can be made configurable)
  std::string filename = extract_filename(local_path);
  evhttp_add_header(out_headers, "Content-Disposition",
                    ("inline; filename=\"" + filename + "\"").c_str());

  // Check If-None-Match (ETag validation)
  if (const char* if_none_match =
          evhttp_find_header(in_headers, "If-None-Match");
      if_none_match && etag == if_none_match) {
    close(fd);
    evhttp_send_reply(req, HTTP_NOTMODIFIED, "Not Modified", nullptr);
    return;
  }

  // Check If-Modified-Since
  if (const char* if_modified =
          evhttp_find_header(in_headers, "If-Modified-Since")) {
    // Check if date was valid AND if file's mtime is less than or equal to
    // the client's cached time. (We use <= to be safe).
    // We also truncate our mtime to seconds, as HTTP dates don't have sub-second precision.
    if (auto client_time = parse_http_date(if_modified);
        client_time.has_value() && (st.st_mtime <= *client_time)) {
      close(fd);
      evhttp_send_reply(req, HTTP_NOTMODIFIED, "Not Modified", nullptr);
      return;
    }
  }

  // Handle empty files
  if (file_size == 0) {
    evhttp_add_header(out_headers, "Content-Length", "0");
    evbuffer* buf = evbuffer_new();
    evhttp_send_reply(req, HTTP_OK, "OK", buf);
    evbuffer_free(buf);
    close(fd);
    return;
  }

  // Parse Range header
  const char* range_hdr = evhttp_find_header(in_headers, "Range");
  auto range = parse_range(range_hdr, file_size);

  // Prepare buffer
  evbuffer* buf = evbuffer_new();
  if (!buf) {
    close(fd);
    evhttp_send_error(req, HTTP_INTERNAL, "Out of memory");
    return;
  }

  int status_code = HTTP_OK;
  off_t offset = 0;
  off_t length = file_size;

  // Process range request
  if (range.has_value()) {
    auto [start, end_inclusive] = *range;

    // Sanity check (should be guaranteed by parse_range with clamping)
    if (start >= file_size || end_inclusive >= file_size || start > end_inclusive) {
      evhttp_add_header(out_headers, "Content-Range",
                        ("bytes */" + std::to_string(file_size)).c_str());
      evbuffer_free(buf);
      close(fd);
      evhttp_send_reply(req, 416, "Range Not Satisfiable", nullptr);
      return;
    }

    offset = start;
    length = end_inclusive - start + 1;

    evhttp_add_header(out_headers, "Content-Range",
                      ("bytes " + std::to_string(start) + "-" +
                       std::to_string(end_inclusive) + "/" +
                       std::to_string(file_size)).c_str());
    status_code = 206; // Partial Content
  }

  evhttp_add_header(out_headers, "Content-Length", std::to_string(length).c_str());

  // Handle HEAD request (headers only, no body)
  if (method == EVHTTP_REQ_HEAD) {
    const char* reason = (status_code == 206) ? "Partial Content" : "OK";
    evhttp_send_reply(req, status_code, reason, buf);
    evbuffer_free(buf);
    close(fd);
    return;
  }

  // Send file using zero-copy file segment
  evbuffer_file_segment* seg = evbuffer_file_segment_new(
      fd, offset, length, EVBUF_FS_CLOSE_ON_FREE);

  if (!seg) {
    evbuffer_free(buf);
    close(fd); // segment didn't take ownership
    evhttp_send_error(req, HTTP_INTERNAL, "Failed to create file segment");
    return;
  }

  if (evbuffer_add_file_segment(buf, seg, 0, length) != 0) {
    evbuffer_file_segment_free(seg); // This will close fd
    evbuffer_free(buf);
    evhttp_send_error(req, HTTP_INTERNAL, "Failed to add file to buffer");
    return;
  }

  // Release our reference to segment (buffer holds its own reference)
  evbuffer_file_segment_free(seg);

  // Send response
  const char* reason = (status_code == 206) ? "Partial Content" : "OK";
  evhttp_send_reply(req, status_code, reason, buf);

  // Cleanup
  evbuffer_free(buf);
  // fd is automatically closed by EVBUF_FS_CLOSE_ON_FREE when buffer is drained
}
#endif  // ANALYZER_FILE_SERVER_H
