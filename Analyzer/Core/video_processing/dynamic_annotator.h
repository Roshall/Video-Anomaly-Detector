#ifndef ANALYZER_DYNAMIC_ANNOTATOR_H
#define ANALYZER_DYNAMIC_ANNOTATOR_H

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <memory>

#include "helper.h"
#include "painter_types.h"
#include "painter_optimized.h"
#include "video_context.h"

struct AVFrame;
struct AVFilterGraph;
struct AVFilterContext;

namespace avs_analyzer {

/**
 * @brief Enhanced annotator for dynamic ROIs and bounding boxes with legend support
 * 
 * This annotator supports:
 * - Dynamic quadrilateral ROIs with different colors
 * - Bounding boxes with different colors
 * - Pre-rendered legend overlay for performance
 * - Batch operations for optimal drawing performance
 */
class DynamicAnnotator {
public:
    struct ROI {
        std::vector<cv::Point> quadrilateral;  // 4-point quadrilateral
        YUVAColor color;
        int thickness = 2;
        std::string label;
    };
    
    struct DetectionBox {
        cv::Rect bbox;
        YUVColor color;
        int thickness = 2;
        std::string label;
    };
    
    struct LegendConfig {
        std::string font_path{"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
        int font_size = 16;
        std::pair<int, int> handle_size = {20, 16};
        int row_spacing = 4;
        int text_padding = 8;
        int frame_padding = 8;
        int frame_thickness = 2;
        YUVAColor frame_color = {210, 128, 128, 255};
        LegendPosition position = LegendPosition::kTopRight;
        int margin_x = 40;
        int margin_y = 40;
    };

public:
    DynamicAnnotator() = default;
    ~DynamicAnnotator() = default;
    
    /**
     * @brief Initialize the annotator with video context and legend configuration
     */
    int init(const VideoContext& video_ctx, const LegendConfig& legend_config);

    int init(VideoContext video_ctx) { return init(video_ctx, LegendConfig()); }
    
    /**
     * @brief Set the ROI and detection box labels (for legend)
     * This creates the legend overlay buffer
     */
    void set_labels(const std::vector<std::string>& roi_labels,
                   const std::vector<std::string>& detection_labels);
    
    /**
     * @brief Redraw the legend in the buffer (call after label changes)
     */
    void update_legend();
    
    /**
     * @brief Draw quadrilaterals, bounding boxes, and overlay legend on frame
     * @param in_frame Input frame (YUV420P)
     * @param rois Vector of ROI quadrilaterals
     * @param detections Vector of detection bounding boxes
     * @return Annotated frame (YUV420P) with legend overlay via FFmpeg filter
     */
    AVFramePtr draw_elements(AVFrame* in_frame,
                            const std::vector<ROI>& rois,
                            const std::vector<DetectionBox>& detections) const;

private:
    VideoContext video_ctx_;
    LegendConfig legend_config_;
    YUVColor bbox_color_;
    
    // FFmpeg overlay filter graph
    AVFilterGraphPtr filter_graph_;
    AVFilterContext* buffersrc_ctx_main_ = nullptr;
    AVFilterContext* buffersrc_ctx_overlay_ = nullptr;
    AVFilterContext* buffersink_ctx_ = nullptr;
    
    // Legend management
    std::vector<std::string> roi_labels_;
    std::vector<std::string> detection_labels_;
    std::vector<YUVAColor> legend_colors_;
    std::vector<std::u8string> legend_labels_utf8_;
    AVFramePtr legend_overlay_frame_;
    LegendLayout legend_layout_;
    
    // Helper functions
    void prepare_legend_data();
    int init_filter_graph();
};

/**
 * @brief Alpha blend overlay region onto destination frame
 * @param dst Destination YUVA420P frame
 * @param overlay Source YUVA420P overlay frame
 * @param dst_x X position in destination
 * @param dst_y Y position in destination
 * @param region_w Width of region (-1 for full overlay width)
 * @param region_h Height of region (-1 for full overlay height)
 */
void overlay_yuva420p_region(AVFrame* dst, const AVFrame* overlay,
                             int dst_x, int dst_y,
                             int region_w = -1, int region_h = -1);

}  // namespace avs_analyzer

#endif  // ANALYZER_DYNAMIC_ANNOTATOR_H