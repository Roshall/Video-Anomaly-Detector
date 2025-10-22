//
// Created by lg on 10/11/25.
//

#ifndef ANALYZER_CONSTANT_YOLO_H
#define ANALYZER_CONSTANT_YOLO_H
#include <memory>
#include "task.h"

namespace avs_analyzer {
class YOLODetector;

std::shared_ptr<const YOLODetector> GetDetector(std::string_view model_path,
                                                Task task, bool use_accelerator);
void ClearYoloDetectorPool();
}


#endif  // ANALYZER_CONSTANT_YOLO_H
