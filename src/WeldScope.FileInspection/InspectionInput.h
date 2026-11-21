#pragma once

#include "BaselineInspection.h"
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace weldscope {
// 파일을 읽기 전에 적용하는 입력 영상과 설정 파일의 최대 바이트 수.
inline constexpr std::size_t image_file_limit = 16 * 1024 * 1024;
inline constexpr std::size_t config_file_limit = 1024 * 1024;

// 화면과 이력에서 같은 원인으로 분류할 수 있는 파일 검사 실패 유형.
enum class FileInspectionFailure { Configuration, FileAccess, SizeLimit, DamagedPng, UnsupportedFormat, Analysis };

// 입력을 거부한 분류와 진단 메시지를 함께 전달한다.
class InspectionInputError : public std::runtime_error {
public:
    // 기계가 읽는 실패 유형과 사람이 읽는 상세 메시지를 보관한다.
    InspectionInputError(FileInspectionFailure reason, const std::string& message)
        : std::runtime_error(message), reason_(reason) {}
    // 예외 문구를 분석하지 않고도 실패 유형으로 분기할 수 있다.
    FileInspectionFailure reason() const noexcept { return reason_; }
private:
    FileInspectionFailure reason_;
};

// 설정에서 읽은 밝기 경계와 불량 비율. 각 알고리즘이 허용 범위를 검사한다.
struct InspectionSettings {
    int threshold;
    double failRatio;
};

// 실행 파일 옆에 있는 기준 검사 설정 경로를 반환한다. 실행 경로를 확인하지 못하면 예외를 던진다.
std::filesystem::path baseline_config_path();
// limit 바이트까지 파일을 그대로 읽는다. 접근 실패와 크기 초과는 InspectionInputError로 구분한다.
std::vector<unsigned char> read_bytes(const std::filesystem::path& path, std::size_t limit);
// 디코딩 전 원본 바이트를 기준으로 소문자 16진수 64자리 지문을 반환한다.
std::string sha256(const std::vector<unsigned char>& bytes);
// 메모리의 UTF-8 JSON을 읽는다. UTF-8 BOM을 허용하며 형식 오류는 예외로 알린다.
cv::FileStorage read_json(const std::vector<unsigned char>& bytes);
// PNG 형식과 끝을 확인하고 최대 1,600만 픽셀까지 디코딩한다. 원래 채널 수와 깊이를 유지한다.
cv::Mat decode_png(const std::vector<unsigned char>& bytes);
// 필수 정수 필드를 int로 읽는다. 누락, 타입 오류, 범위 초과는 필드 이름과 함께 알린다.
int read_integer(const cv::FileNode& node, const char* name);
// 스키마 버전 1에서 지정한 경계 키와 수치형 fail_ratio를 읽는다.
InspectionSettings read_inspection_settings(const cv::FileStorage& json, const char* thresholdKey);
// intensity_threshold와 fail_ratio를 읽어 값의 범위까지 확인한 기준 검사 설정을 만든다.
BaselineConfig parse_baseline_config(const cv::FileStorage& json);
// BGR 원본과 같은 크기의 8비트 단일 채널 마스크를 받는다. 원본을 보존하고 색을 입힌 복사본을 반환한다.
cv::Mat make_candidate_overlay(const cv::Mat& bgr, const cv::Mat& mask);
}
