#pragma once

#include "ByteTrack/Rect.h"

namespace byte_track
{
struct Object
{
    Rect<float> rect;
    int label;
    float prob;
};
}