//
// Created by lg on 11/12/25.
//

#ifndef ANALYZER_PAINTER_TYPES_H
#define ANALYZER_PAINTER_TYPES_H
#include <cstdint>
namespace avs_analyzer {
struct YUVAColor {
  uint8_t y, u, v, a;
};

struct YUVColor {
  uint8_t y, u, v;
};
}

#endif //ANALYZER_PAINTER_TYPES_H