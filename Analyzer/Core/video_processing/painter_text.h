#ifndef ANALYZER_PAINTER_TEXT_H
#define ANALYZER_PAINTER_TEXT_H

#include "painter_base.h"
#include "painter_types.h"
#include <string>
#include <vector>


// Forward declarations for FreeType
struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef FT_LibraryRec_* FT_Library;
typedef FT_FaceRec_* FT_Face;

namespace avs_analyzer {
// UTF-8 utilities
namespace utf8 {
bool decode_next(const std::string& str, size_t& index, uint32_t& codepoint);
bool decode_next(const std::u8string& str, size_t& index, uint32_t& codepoint);
int measure_text_width(FT_Face face, const std::string& text);
int measure_text_width(FT_Face face, const std::u8string& text);
}

// FreeType resource management
class FontManager {
public:
  FontManager();
  ~FontManager();

  // Non-copyable
  FontManager(const FontManager&) = delete;
  FontManager& operator=(const FontManager&) = delete;

  // Movable
  FontManager(FontManager&&) noexcept;
  FontManager& operator=(FontManager&&) noexcept;

  bool load_font(const std::string& font_path, int font_size);
  [[nodiscard]] FT_Face get_face() const noexcept { return face_; }
  [[nodiscard]] bool is_valid() const noexcept { return library_ && face_; }

private:
  FT_Library library_;
  FT_Face face_;

  void cleanup();
};

// Text rendering functions
void render_glyph_to_yuva420p(AVFrame* frame, int dest_x, int dest_y,
                              const struct FT_Bitmap_& bitmap,
                              const YUVAColor& color,
                              bool write_chroma = true);

void render_text_yuva420p(AVFrame* frame, FT_Face face,
                          int x, int y, const std::string& text,
                          const YUVAColor& color,
                          bool write_chroma = true);

void render_text_yuva420p(AVFrame* frame, FT_Face face,
                          int x, int y, const std::u8string& text,
                          const YUVAColor& color,
                          bool write_chroma = true);

// Legend drawing
struct LegendStyle {
  int text_padding = 8;
  int frame_padding = 8;
  int frame_thickness = 2;
  bool draw_frame = true;
  YUVAColor frame_color = {210, 128, 128, 255};
};

void draw_legend_yuva420p(AVFrame* frame,
                                    const std::string& font_path,
                                    std::pair<int, int> origin,
                                    int font_size,
                                    std::pair<int, int> handle_size,
                                    int row_spacing,
                                    const std::vector<YUVAColor>& colors,
                                    const std::vector<std::u8string>& labels,
                                    const LegendStyle& style = {},
                                    bool write_chroma = true);
} // namespace avs_analyzer

#endif  // ANALYZER_PAINTER_TEXT_H