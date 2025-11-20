#include "dynamic_annotator.h"
#include "task.h"
#include <iostream>

// Example usage of the DynamicAnnotator
void example_usage() {
    using namespace avs_analyzer;
    
    // Initialize video context
    VideoContext ctx;
    ctx.width = 1920;
    ctx.height = 1080;
    
    // Create and initialize annotator. Note: init() will pre-allocate the
    // legend overlay using the full Task enum set and create the FFmpeg
    // overlay filter graph once; subsequent init() calls with a different
    // VideoContext will be rejected to enforce single-allocation semantics.
    DynamicAnnotator annotator;
    DynamicAnnotator::LegendConfig legend_config;
    legend_config.position = LegendPosition::kTopRight;
    legend_config.font_size = 18;
    
    if (annotator.init(ctx, legend_config) < 0) {
        std::cerr << "Failed to initialize annotator" << std::endl;
        return;
    }
    
    // Set up tasks for legend
    std::vector<Task> tasks = {Task::kPhonePlaying, Task::kSleepWork, Task::kWandering, Task::kBreakIn};
    // Update legend once with global task list
    annotator.update_legend(tasks);
    
    // Example usage in frame processing loop:
    /*
    AVFrame* input_frame = ...; // Your input frame
    
    // Define ROIs (task + quadrilateral)
    std::vector<DynamicAnnotator::ROIEntry> rois;
    rois.emplace_back(Task::kPhonePlaying,
                      std::vector<cv::Point>{{100, 100}, {300, 100}, {300, 200}, {100, 200}});
    
    // Define detections (task + bounding box)
    std::vector<DynamicAnnotator::BBoxEntry> detections;
    detections.emplace_back(Task::kSleepWork, cv::Rect(150, 150, 100, 150));
    
    // Annotate frame
    auto annotated_frame = annotator.draw_elements(input_frame, rois, detections);
    
    if (annotated_frame) {
        // Use annotated frame (YUVA420P format with alpha channel)
        // ...
    }
    */
}