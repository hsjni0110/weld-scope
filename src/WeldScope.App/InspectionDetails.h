#pragma once

#include "InspectionHistory.h"

#include <opencv2/core.hpp>
#include <string>

namespace weldscope {

// 단일 검사 이력은 원본을 연다. 폴더 결과는 저장된 설정으로 후보 영상도 복원한다.
enum class InspectionDetailSource { SingleFileHistory,
                                    FolderResult };

// 저장한 원본을 다시 열거나 후보를 복원했을 때의 상황. 화면이 표시 문구를 정한다.
enum class InspectionSourceStatus { Available,
                                    Unverifiable,
                                    Changed,
                                    Missing,
                                    Unreadable,
                                    OverlaySettingsUnavailable };

// 조회한 검사 기록과 표시용 BGRA 영상. 원본 확인에 실패하면 영상이 비어 있다.
struct OpenedInspection {
    InspectionRecord record;
    cv::Mat image, overlay;
    InspectionSourceStatus sourceStatus = InspectionSourceStatus::Available;
};

// 호출 스레드에서 DB와 파일을 읽는다. 기록 자체가 없거나 DB 조회에 실패하면 예외를 던진다.
// 원본 또는 후보를 열 수 없는 경우에는 저장 기록과 원인을 반환한다.
OpenedInspection ReadInspectionDetails(const std::string& inspectionId, InspectionDetailSource source);

} // namespace weldscope
