#pragma once
#include "SelectiveInspectionPolicy.h"
#include <algorithm>

namespace pooled_texture {
// 폭에 따라 두 판정 구간 사이의 통과 및 불량 경계를 선형으로 보간한다.
// width의 단위는 픽셀이다. 두 폭 경계에서 6픽셀 이내일 때만 구간을 섞는다.
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
