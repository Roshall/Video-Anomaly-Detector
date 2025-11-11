//
// Created by lg on 11/7/25.
//

#ifndef ANALYZER_PAINTER_H
#define ANALYZER_PAINTER_H
// Legend drawer for AV_PIX_FMT_YUVA420P using FreeType2
// Inputs:
//   frame            - AVFrame* in AV_PIX_FMT_YUVA420P (writable)
//   font_path        - path to .ttf / .otf
//   origin           - (x,y) top-left of the legend block
//   font_px          - font pixel height (FreeType "pixel size")
//   handle_wh        - (width, height) of the colored rectangle handle
//   row_spacing      - extra pixels between rows (>=0)
//   colors           - vector<YUVAColor> (y,u,v,a) for each legend item
//   labels_utf8      - vector<string> UTF-8, aligned with colors
// Notes:
//   - For crisp text edges, we set YUV to the legend color and take glyph coverage into A.
//   - If you want colored chroma-less text (sharper), you can skip painting U/V per pixel.
//   - This draws into a YUVA *overlay* (alpha used later by the overlay filter).
//     If you draw onto your final image (no later blending), alpha has no effect visually.

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <ft2build.h>
#include FT_FREETYPE_H

struct YUVAColor { uint8_t y, u, v, a; };
struct ChromaInfo {
    unsigned x;
    unsigned y;
    unsigned width;
    unsigned height;
};

static inline bool in_bounds(int x, int y, int width, int height) {
    return static_cast<unsigned>(x) < static_cast<unsigned>(width)
        && static_cast<unsigned>(y) < static_cast<unsigned>(height);
}

// Get chroma plane information for 4:2:0 subsampling
static inline ChromaInfo get_chroma_info(const AVFrame* frame, int x, int y) {
    return {
        static_cast<unsigned>(x >> 1),
        static_cast<unsigned>(y >> 1),
        static_cast<unsigned>((frame->width + 1) >> 1),
        static_cast<unsigned>((frame->height + 1) >> 1)
    };
}

// Set chroma pixels if within bounds
static inline void set_chroma_pixels(AVFrame* frame, unsigned chroma_x, unsigned chroma_y,
                                   uint8_t u_value, uint8_t v_value) {
    if (chroma_x < get_chroma_info(frame, frame->width, 0).width &&
        chroma_y < get_chroma_info(frame, 0, frame->height).height) {
        frame->data[1][chroma_y * frame->linesize[1] + chroma_x] = u_value;
        frame->data[2][chroma_y * frame->linesize[2] + chroma_x] = v_value;
    }
}

// Common pixel writing logic
static inline void write_yuva_pixel(AVFrame* frame, int x, int y, const YUVAColor& color,
                                  bool write_chroma = true) {
    if (!in_bounds(x, y, frame->width, frame->height)) return;

    // Luma plane
    frame->data[0][y * frame->linesize[0] + x] = color.y;

    // Alpha plane
    frame->data[3][y * frame->linesize[3] + x] = color.a;

    if (write_chroma) {
        const auto chroma = get_chroma_info(frame, x, y);
        set_chroma_pixels(frame, chroma.x, chroma.y, color.u, color.v);
    }
}

// Blend alpha with coverage scaled by color alpha
static inline void write_alpha_coverage(AVFrame* frame, int x, int y, uint8_t coverage, uint8_t color_alpha) {
    if (!in_bounds(x, y, frame->width, frame->height)) return;

    uint8_t* alpha_ptr = frame->data[3] + y * frame->linesize[3] + x;
    const uint32_t blended_alpha = (uint32_t)coverage * (uint32_t)color_alpha / 255u;

    // Keep maximum alpha for overlapping elements
    *alpha_ptr = std::max(*alpha_ptr, static_cast<uint8_t>(blended_alpha));
}

// Draw filled rectangle
static void fill_rect_yuva420p(AVFrame* frame, int x, int y, int width, int height,
                              const YUVAColor& color, bool write_chroma = true) {
    const int x_start = std::max(0, x);
    const int y_start = std::max(0, y);
    const int x_end = std::min(frame->width, x + width);
    const int y_end = std::min(frame->height, y + height);

    // Luma and alpha planes (full resolution)
    for (int row = y_start; row < y_end; ++row) {
        for (int col = x_start; col < x_end; ++col) {
            frame->data[0][row * frame->linesize[0] + col] = color.y;

            uint8_t& alpha_ref = frame->data[3][row * frame->linesize[3] + col];
            alpha_ref = std::max(alpha_ref, color.a);
        }
    }

    if (write_chroma) {
        // Chroma planes (half resolution)
        const int chroma_x_start = x_start >> 1;
        const int chroma_y_start = y_start >> 1;
        const int chroma_x_end = (x_end + 1) >> 1;
        const int chroma_y_end = (y_end + 1) >> 1;

        for (int chroma_row = chroma_y_start; chroma_row < chroma_y_end; ++chroma_row) {
            for (int chroma_col = chroma_x_start; chroma_col < chroma_x_end; ++chroma_col) {
                set_chroma_pixels(frame, chroma_col, chroma_row, color.u, color.v);
            }
        }
    }
}

// Draw rectangle outline
static void draw_rect_outline_yuva420p(AVFrame* frame, int x, int y, int width, int height,
                                      const YUVAColor& color, int thickness = 2, bool write_chroma = true) {
    if (thickness <= 0 || width <= 0 || height <= 0) return;

    // Top, bottom, left, right strips
    fill_rect_yuva420p(frame, x, y, width, thickness, color, write_chroma);
    fill_rect_yuva420p(frame, x, y + height - thickness, width, thickness, color, write_chroma);
    fill_rect_yuva420p(frame, x, y, thickness, height, color, write_chroma);
    fill_rect_yuva420p(frame, x + width - thickness, y, thickness, height, color, write_chroma);
}

// UTF-8 to codepoint decoder
static bool utf8_next(const std::u8string& str, size_t& index, uint32_t& codepoint) {
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

// Render glyph bitmap to YUVA frame
static void blit_glyph_gray_to_yuva420p(AVFrame* frame, int dest_x, int dest_y,
                                       const FT_Bitmap& bitmap, const YUVAColor& color,
                                       bool write_chroma = true) {
    for (int row = 0; row < bitmap.rows; ++row) {
        for (int col = 0; col < bitmap.width; ++col) {
            const int pixel_x = dest_x + col;
            const int pixel_y = dest_y + row;

            if (!in_bounds(pixel_x, pixel_y, frame->width, frame->height)) continue;

            const uint8_t coverage = bitmap.buffer[row * bitmap.pitch + col];
            if (coverage == 0) continue;

            // Set luma to text color
            frame->data[0][pixel_y * frame->linesize[0] + pixel_x] = color.y;

            // Blend alpha with glyph coverage
            write_alpha_coverage(frame, pixel_x, pixel_y, coverage, color.a);

            if (write_chroma) {
                const auto chroma = get_chroma_info(frame, pixel_x, pixel_y);
                set_chroma_pixels(frame, chroma.x, chroma.y, color.u, color.v);
            }
        }
    }
}

// Measure text width using FreeType advances
static int measure_text_width(FT_Face face, const std::u8string& utf8_text) {
    int width = 0;
    size_t index = 0;
    uint32_t codepoint;

    while (utf8_next(utf8_text, index, codepoint)) {
        if (FT_Load_Char(face, codepoint, FT_LOAD_DEFAULT)) continue;
        width += static_cast<int>(face->glyph->advance.x >> 6);
    }

    return width;
}

// Initialize FreeType and load font face
static bool initialize_freetype(FT_Library& library, FT_Face& face, const std::string& font_path, int font_size) {
    if (FT_Init_FreeType(&library)) return false;
    if (FT_New_Face(library, font_path.c_str(), 0, &face)) {
        FT_Done_FreeType(library);
        return false;
    }
    if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(font_size))) {
        FT_Done_Face(face);
        FT_Done_FreeType(library);
        return false;
    }
    return true;
}

// Calculate legend dimensions
static std::tuple<int, int, int, int> calculate_legend_dimensions(
    FT_Face face, const std::vector<std::u8string>& labels,
    std::pair<int, int> origin, std::pair<int, int> handle_size,
    int font_size, int row_spacing, int text_padding, int frame_padding) {

    const int start_x = origin.first;
    const int start_y = origin.second;
    const int handle_width = std::max(0, handle_size.first);
    const int handle_height = std::max(0, handle_size.second);
    const int row_height = std::max(handle_height, font_size);

    // Measure text dimensions
    int max_text_width = 0;
    for (const auto& label : labels) {
        max_text_width = std::max(max_text_width, measure_text_width(face, label));
    }

    // Calculate content dimensions
    const int item_count = static_cast<int>(labels.size());
    const int content_width = handle_width + text_padding + max_text_width;
    const int content_height = item_count > 0
        ? row_height * item_count + std::max(0, row_spacing) * (item_count - 1)
        : 0;

    // Calculate frame dimensions
    int frame_x = std::max(0, start_x - frame_padding);
    int frame_y = std::max(0, start_y - frame_padding - font_size / 2);
    int frame_width = content_width + 2 * frame_padding;
    int frame_height = content_height + 2 * frame_padding;

    return {frame_x, frame_y, frame_width, frame_height};
}

// Draw a single legend item
static void draw_legend_item(AVFrame* frame, FT_Face face, int start_x, int current_y,
                           const YUVAColor& color, const std::u8string& label,
                           int handle_width, int handle_height, int font_size,
                           int text_padding, bool write_chroma) {

    // Draw color handle
    fill_rect_yuva420p(frame, start_x, current_y, handle_width, handle_height, color, write_chroma);

    // Draw text label
    const int text_x = start_x + handle_width + text_padding;
    const int baseline_y = current_y + font_size / 2;

    size_t char_index = 0;
    uint32_t codepoint;
    int pen_x = text_x;

    while (utf8_next(label, char_index, codepoint)) {
        if (FT_Load_Char(face, codepoint, FT_LOAD_RENDER)) continue;

        FT_GlyphSlot glyph = face->glyph;
        const int glyph_x = pen_x + glyph->bitmap_left;
        const int glyph_y = baseline_y - glyph->bitmap_top;

        blit_glyph_gray_to_yuva420p(frame, glyph_x, glyph_y, glyph->bitmap, color, write_chroma);
        pen_x += static_cast<int>(glyph->advance.x >> 6);
    }
}

// Main legend drawing function
inline void draw_legend_yuva420p(AVFrame* frame,
                                const std::string& font_path,
                                std::pair<int, int> origin,
                                int font_size,
                                std::pair<int, int> handle_size,
                                int row_spacing,
                                const std::vector<YUVAColor>& colors,
                                const std::vector<std::u8string>& labels,
                                bool write_chroma = true,
                                int text_padding = 8,
                                bool draw_frame = true,
                                int frame_padding = 8,
                                int frame_thickness = 2,
                                YUVAColor frame_color = {210, 128, 128, 255}) {

    // Validate inputs
    if (!frame || frame->format != AV_PIX_FMT_YUVA420P) {
        throw std::runtime_error("Frame must be in AV_PIX_FMT_YUVA420P format");
    }

    if (colors.size() != labels.size()) {
        throw std::runtime_error("Colors and labels must have the same size");
    }

    // Initialize FreeType
    FT_Library free_type;
    FT_Face face;
    if (!initialize_freetype(free_type, face, font_path, font_size)) {
        throw std::runtime_error("Failed to initialize FreeType or load font: " + font_path);
    }

    const int start_x = origin.first;
    const int handle_width = std::max(0, handle_size.first);
    const int handle_height = std::max(0, handle_size.second);
    const int row_height = std::max(handle_height, font_size);
    const int row_step = row_height + std::max(0, row_spacing);

    // Calculate dimensions
    auto [frame_x, frame_y, frame_width, frame_height] = calculate_legend_dimensions(
        face, labels, origin, handle_size, font_size, row_spacing, text_padding, frame_padding);

    // Clamp to frame boundaries
    frame_width = std::min(frame_width, frame->width - frame_x);
    frame_height = std::min(frame_height, frame->height - frame_y);

    // Draw legend items
    int current_y = origin.second;
    const int item_count = static_cast<int>(labels.size());

    for (int i = 0; i < item_count; ++i) {
        if (current_y >= frame->height) break;
        draw_legend_item(frame, face, start_x, current_y, colors[i], labels[i],
                        handle_width, handle_height, font_size, text_padding, write_chroma);
        current_y += row_step;
    }

    // Draw frame around legend
    if (draw_frame && frame_width > 0 && frame_height > 0 && frame_thickness > 0) {
        draw_rect_outline_yuva420p(frame, frame_x, frame_y, frame_width, frame_height,
                                  frame_color, frame_thickness, true);
    }

    // Cleanup FreeType resources
    FT_Done_Face(face);
    FT_Done_FreeType(free_type);
}

#endif  // ANALYZER_PAINTER_H
