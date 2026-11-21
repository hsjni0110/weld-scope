#pragma once
#include "SelectiveInspectionPolicy.h"
#include <algorithm>

namespace pooled_texture {
// Endpoints are inherited from the trained width calibrations. A six-pixel
// radius spans one horizontal sampling pitch around either routing boundary.
inline weldscope::ScoreBand interpolated_band(int width, weldscope::ScoreBand inside, weldscope::ScoreBand outside)
{
    constexpr double radius=6;
    const double distance=std::min(width-384.5,415.5-width);
    if (distance>=radius) return inside;
    if (distance<=-radius) return outside;
    const double weight=(distance+radius)/(2*radius);
    return {outside.passMax+weight*(inside.passMax-outside.passMax),
            outside.failMin+weight*(inside.failMin-outside.failMin)};
}
}
