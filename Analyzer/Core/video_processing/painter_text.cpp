#include "painter_text.h"
#include "painter_shapes.h"
#include "freetype/freetype.h"
#include "freetype/ftimage.h"
#include <stdexcept>
#include <algorithm>

#include "Utils/log.h"


namespace avs_analyzer {
namespace utf8 {
bool decode_next(const std::string& str, size_t& index, uint32_t& codepoint) {
  if (index >= str.size()) return false;

  const auto c = static_cast<unsigned char>(str[index++]);

  // Single byte character (ASCII)
  if (c < 0x80) {
    codepoint = c;
    return true;
  }

  // Multi-byte character
  int extra_bytes = -1;
  uint32_t mask = 0;

  if ((c & 0xE0) == 0xC0) {
    extra_bytes = 1;
    mask = 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    extra_bytes = 2;
    mask = 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    extra_bytes = 3;
    mask = 0x07;
  }

  if (extra_bytes < 0 || index + extra_bytes > str.size()) {
    return false;
  }

  codepoint = c & mask;

  for (int i = 0; i < extra_bytes; ++i) {
    const auto trail_byte = static_cast<unsigned char>(str[index++]);
    if ((trail_byte & 0xC0) != 0x80) return false;
    codepoint = (codepoint << 6) | (trail_byte & 0x3F);
  }

  return true;
}

bool decode_next(const std::u8string& str, size_t& index, uint32_t& codepoint) {
  if (index >= str.size()) return false;

  const char8_t lead_byte = str[index++];

  // Single byte character (ASCII)
  if (lead_byte < 0x80) {
    codepoint = lead_byte;
    return true;
  }

  // Multi-byte character
  int extra_bytes = -1;
  if ((lead_byte & 0xE0) == 0xC0) extra_bytes = 1;
  else if ((lead_byte & 0xF0) == 0xE0) extra_bytes = 2;
  else if ((lead_byte & 0xF8) == 0xF0) extra_bytes = 3;

  if (extra_bytes < 0 || index + extra_bytes > str.size()) return false;

  codepoint = lead_byte & (0x1F >> (extra_bytes - 1));

  for (int i = 0; i < extra_bytes; ++i) {
    const char8_t trail_byte = str[index++];
    if ((trail_byte & 0xC0) != 0x80) return false;
    codepoint = (codepoint << 6) | (trail_byte & 0x3F);
  }

  return true;
}

int measure_text_width(FT_Face face, const std::string& text) {
  int width = 0;
  size_t index = 0;
  uint32_t codepoint;

  while (decode_next(text, index, codepoint)) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT)) continue;
    width += static_cast<int>(face->glyph->advance.x >> 6);
  }

  return width;
}

int measure_text_width(FT_Face face, const std::u8string& text) {
  int width = 0;
  size_t index = 0;
  uint32_t codepoint;

  while (decode_next(text, index, codepoint)) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT)) continue;
    width += static_cast<int>(face->glyph->advance.x >> 6);
  }

  return width;
}
} // namespace utf8

FontManager::FontManager() : library_(nullptr), face_(nullptr) {
}

FontManager::~FontManager() {
  cleanup();
}

FontManager::FontManager(FontManager&& other) noexcept
  : library_(other.library_), face_(other.face_) {
  other.library_ = nullptr;
  other.face_ = nullptr;
}

FontManager& FontManager::operator=(FontManager&& other) noexcept {
  if (this != &other) {
    cleanup();
    library_ = other.library_;
    face_ = other.face_;
    other.library_ = nullptr;
    other.face_ = nullptr;
  }
  return *this;
}

bool FontManager::load_font(const std::string& font_path, int font_size) {
  cleanup();

  if (FT_Init_FreeType(&library_)) {
    LOGW("Failed to initialize FreeType library");
    return false;
  }

  if (FT_New_Face(library_, font_path.c_str(), 0, &face_)) {
    LOGW("Failed to load font: {}", font_path);
    FT_Done_FreeType(library_);
    library_ = nullptr;
    return false;
  }

  if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(font_size))) {
    LOGW("Failed to set font size: {}", font_size);
    cleanup();
    return false;
  }

  return true;
}

void FontManager::cleanup() {
  if (face_) {
    FT_Done_Face(face_);
    face_ = nullptr;
  }
  if (library_) {
    FT_Done_FreeType(library_);
    library_ = nullptr;
  }
}

void render_glyph_to_yuva420p(AVFrame* frame, int dest_x, int dest_y,
                              const FT_Bitmap& bitmap,
                              const YUVAColor& color,
                              bool write_chroma) {
  if (!frame || !is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) {
    return;
  }

  for (int row = 0; row < bitmap.rows; ++row) {
    for (int col = 0; col < bitmap.width; ++col) {
      const int pixel_x = dest_x + col;
      const int pixel_y = dest_y + row;

      if (!in_bounds(pixel_x, pixel_y, frame->width, frame->height)) continue;

      const uint8_t coverage = bitmap.buffer[row * bitmap.pitch + col];
      if (coverage == 0) continue;

      // Set luma
      frame->data[0][pixel_y * frame->linesize[0] + pixel_x] = color.y;

      // Blend alpha with coverage
      uint8_t* alpha_ptr = frame->data[3] + pixel_y * frame->linesize[3] +
                           pixel_x;
      const auto blended_alpha = static_cast<uint32_t>(coverage) *
                                 static_cast<uint32_t>(color.a) / 255u;
      *alpha_ptr = std::max(*alpha_ptr, static_cast<uint8_t>(blended_alpha));

      if (write_chroma) {
        const auto chroma = get_chroma_info(frame, pixel_x, pixel_y);
        if (chroma.x < chroma.width && chroma.y < chroma.height) {
          frame->data[1][chroma.y * frame->linesize[1] + chroma.x] = color.u;
          frame->data[2][chroma.y * frame->linesize[2] + chroma.x] = color.v;
        }
      }
    }
  }
}

void render_text_yuva420p(AVFrame* frame, FT_Face face,
                          int x, int y, const std::string& text,
                          const YUVAColor& color,
                          bool write_chroma) {
  if (!frame || !face) return;

  size_t char_index = 0;
  uint32_t codepoint;
  int pen_x = x;

  while (utf8::decode_next(text, char_index, codepoint)) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) continue;

    FT_GlyphSlot glyph = face->glyph;
    const int glyph_x = pen_x + glyph->bitmap_left;
    const int glyph_y = y - glyph->bitmap_top;

    render_glyph_to_yuva420p(frame, glyph_x, glyph_y, glyph->bitmap,
                             color, write_chroma);
    pen_x += static_cast<int>(glyph->advance.x >> 6);
  }
}

void render_text_yuva420p(AVFrame* frame, FT_Face face,
                          int x, int y, const std::u8string& text,
                          const YUVAColor& color,
                          bool write_chroma) {
  if (!frame || !face) return;

  size_t char_index = 0;
  uint32_t codepoint;
  int pen_x = x;

  while (utf8::decode_next(text, char_index, codepoint)) {
    if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) continue;

    FT_GlyphSlot glyph = face->glyph;
    const int glyph_x = pen_x + glyph->bitmap_left;
    const int glyph_y = y - glyph->bitmap_top;

    render_glyph_to_yuva420p(frame, glyph_x, glyph_y, glyph->bitmap,
                             color, write_chroma);
    pen_x += static_cast<int>(glyph->advance.x >> 6);
  }
}

void draw_legend_yuva420p_optimized(AVFrame* frame,
                                    const std::string& font_path,
                                    std::pair<int, int> origin,
                                    int font_size,
                                    std::pair<int, int> handle_size,
                                    int row_spacing,
                                    const std::vector<YUVAColor>& colors,
                                    const std::vector<std::u8string>& labels,
                                    const LegendStyle& style,
                                    bool write_chroma) {
  if (!frame || colors.size() != labels.size()) {
    throw std::runtime_error("Invalid input parameters for legend drawing");
  }

  if (!is_yuva420_format(static_cast<AVPixelFormat>(frame->format))) {
    throw std::runtime_error("Frame must be in AV_PIX_FMT_YUVA420P format");
  }

  // Initialize font
  FontManager font_manager;
  if (!font_manager.load_font(font_path, font_size)) {
    throw std::runtime_error("Failed to load font: " + font_path);
  }

  FT_Face face = font_manager.get_face();

  const int start_x = origin.first;
  int current_y = origin.second;
  const int handle_width = std::max(0, handle_size.first);
  const int handle_height = std::max(0, handle_size.second);
  const int row_height = std::max(handle_height, font_size);
  const int row_step = row_height + std::max(0, row_spacing);

  // Calculate legend dimensions for frame
  int max_text_width = 0;
  for (const auto& label : labels) {
    max_text_width = std::max(max_text_width,
                              utf8::measure_text_width(face, label));
  }

  const int content_width = handle_width + style.text_padding + max_text_width;
  const int content_height = labels.empty()
                               ? 0
                               : static_cast<int>(labels.size()) * row_height +
                                 std::max(0, row_spacing) * (
                                   static_cast<int>(labels.size()) - 1);

  // Draw frame if requested
  if (style.draw_frame && content_width > 0 && content_height > 0) {
    const cv::Rect frame_rect(
        start_x - style.frame_padding,
        current_y - style.frame_padding,
        content_width + 2 * style.frame_padding,
        content_height + 2 * style.frame_padding
        );

    draw_rectangle_yuva420p_optimized(frame, frame_rect, style.frame_color,
                                      style.frame_thickness, write_chroma);
  }

  // Draw legend items
  for (size_t i = 0; i < colors.size() && i < labels.size(); ++i) {
    if (current_y >= frame->height) break;

    // Draw color handle
    const cv::Rect handle_rect(start_x, current_y, handle_width, handle_height);
    draw_filled_rectangle_yuva420p_optimized(frame, handle_rect, colors[i],
                                             write_chroma);

    // Draw text label
    const int text_x = start_x + handle_width + style.text_padding;
    const int baseline_y = current_y + font_size / 2;

    render_text_yuva420p(frame, face, text_x, baseline_y, labels[i], colors[i],
                         write_chroma);

    current_y += row_step;
  }
}
} // namespace avs_analyzer