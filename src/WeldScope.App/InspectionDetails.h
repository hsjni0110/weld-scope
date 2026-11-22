#pragma once

#include "InspectionHistory.h"

#include <opencv2/core.hpp>
#include <string>

namespace weldscope {

// 상세를 표시할 화면을 구분한다.
enum class InspectionDetailSource { SingleFileHistory,
                                    FolderResult };

// 저장한 원본을 다시 열었을 때의 파일 상태.
enum class InspectionSourceStatus { Available,
                                    Unverifiable,
                                    Changed,
                                    Missing,
                                    Unreadable };

// 조회한 검사 기록과 표시용 BGRA 영상. 원본 확인에 실패하면 영상이 비어 있다.
struct OpenedInspection {
    InspectionRecord record;
    cv::Mat image;
    InspectionSourceStatus sourceStatus = InspectionSourceStatus::Available;
};

// 호출 스레드에서 DB와 파일을 읽는다. 기록 자체가 없거나 DB 조회에 실패하면 예외를 던진다.
// 원본을 열 수 없는 경우에는 저장 기록과 원인을 반환한다.
OpenedInspection ReadInspectionDetails(const std::string& inspectionId);

} // namespace weldscope
