#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <opencv2/core.hpp>

namespace weldscope {
std::vector<unsigned char> read_bytes(const std::filesystem::path& path, std::size_t limit);
std::string sha256(const std::vector<unsigned char>& bytes);
cv::FileStorage read_json(const std::vector<unsigned char>& bytes);
cv::Mat decode_png(const std::vector<unsigned char>& bytes);
void write_png(const std::filesystem::path& path, const cv::Mat& image);
void write_text(const std::filesystem::path& path, const std::string& text);
std::string csv_field(const std::string& text);
std::filesystem::path contained_path(const std::filesystem::path& root, const std::string& relative);
}
