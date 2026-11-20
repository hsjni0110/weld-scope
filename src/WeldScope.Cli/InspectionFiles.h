#pragma once
#include <filesystem>
#include <string>
#include <opencv2/core.hpp>

namespace weldscope {
void write_png(const std::filesystem::path& path, const cv::Mat& image);
void write_text(const std::filesystem::path& path, const std::string& text);
std::string csv_field(const std::string& text);
std::filesystem::path contained_path(const std::filesystem::path& root, const std::string& relative);
}
