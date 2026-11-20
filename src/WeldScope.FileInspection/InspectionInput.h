#pragma once

#include "BaselineInspection.h"
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace weldscope {
inline constexpr std::size_t image_file_limit = 16 * 1024 * 1024;
inline constexpr std::size_t config_file_limit = 1024 * 1024;

enum class FileInspectionFailure { Configuration, FileAccess, SizeLimit, DamagedPng, UnsupportedFormat, Analysis };

class InspectionInputError : public std::runtime_error {
public:
    InspectionInputError(FileInspectionFailure reason, const std::string& message)
        : std::runtime_error(message), reason_(reason) {}
    FileInspectionFailure reason() const noexcept { return reason_; }
private:
    FileInspectionFailure reason_;
};

struct InspectionSettings {
    int threshold;
    double failRatio;
};

std::vector<unsigned char> read_bytes(const std::filesystem::path& path, std::size_t limit);
std::string sha256(const std::vector<unsigned char>& bytes);
cv::FileStorage read_json(const std::vector<unsigned char>& bytes);
cv::Mat decode_png(const std::vector<unsigned char>& bytes);
int read_integer(const cv::FileNode& node, const char* name);
InspectionSettings read_inspection_settings(const cv::FileStorage& json, const char* thresholdKey);
BaselineConfig parse_baseline_config(const cv::FileStorage& json);
cv::Mat make_candidate_overlay(const cv::Mat& bgr, const cv::Mat& mask);
}
