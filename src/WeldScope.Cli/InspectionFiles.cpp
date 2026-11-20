#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "InspectionFiles.h"
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace weldscope {
std::vector<unsigned char> read_bytes(const std::filesystem::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("File open failed: " + path.u8string());
    const auto size = input.tellg();
    if (size < 0 || static_cast<unsigned long long>(size) > limit) throw std::runtime_error("Input size limit exceeded");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (size && !input.read(reinterpret_cast<char*>(bytes.data()), size)) throw std::runtime_error("Input read failed");
    return bytes;
}

std::string sha256(const std::vector<unsigned char>& bytes)
{
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle = nullptr;
        ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm;
    if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA-256 provider unavailable");
    unsigned char digest[32];
    if (BCryptHash(algorithm.handle, nullptr, 0, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), digest, sizeof(digest)) < 0)
        throw std::runtime_error("SHA-256 failed");
    std::ostringstream text;
    text << std::hex << std::setfill('0');
    for (auto value : digest) text << std::setw(2) << static_cast<int>(value);
    return text.str();
}

cv::FileStorage read_json(const std::vector<unsigned char>& bytes)
{
    std::string text(bytes.begin(), bytes.end());
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    cv::FileStorage json(text, cv::FileStorage::READ | cv::FileStorage::MEMORY | cv::FileStorage::FORMAT_JSON);
    if (!json.isOpened()) throw std::runtime_error("JSON read failed");
    return json;
}

cv::Mat decode_png(const std::vector<unsigned char>& bytes)
{
    const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || !std::equal(std::begin(signature), std::end(signature), bytes.begin())
        || bytes[8] != 0 || bytes[9] != 0 || bytes[10] != 0 || bytes[11] != 13
        || !std::equal(bytes.begin() + 12, bytes.begin() + 16, "IHDR"))
        throw std::invalid_argument("Unsupported image: expected PNG with IHDR");
    auto dimension = [&](std::size_t offset) {
        return (static_cast<std::uint64_t>(bytes[offset]) << 24) | (static_cast<std::uint64_t>(bytes[offset + 1]) << 16)
            | (static_cast<std::uint64_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
    };
    const auto width = dimension(16), height = dimension(20);
    if (!width || !height || width * height > 16000000)
        throw std::invalid_argument("PNG pixel limit exceeded: 16000000");
    // Only the header bound is checked here; OpenCV validates the PNG stream and its pixels.
    return cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
}

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
}

void write_png(const std::filesystem::path& path, const cv::Mat& image)
{
    std::vector<unsigned char> encoded;
    if (!cv::imencode(".png", image, encoded)) throw std::runtime_error("PNG encoding failed");
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    output.close();
}

std::string csv_field(const std::string& text)
{
    std::string quoted = "\"";
    for (char character : text) { if (character == '"') quoted += '"'; quoted += character; }
    return quoted + '"';
}

std::filesystem::path contained_path(const std::filesystem::path& root, const std::string& relative)
{
    const auto path = std::filesystem::u8path(relative);
    if (relative.empty() || path.is_absolute() || relative.find_first_of("\\:\r\n") != std::string::npos)
        throw std::invalid_argument("Unsafe input path");
    for (const auto& part : path) if (part == ".." || part == ".") throw std::invalid_argument("Unsafe input path");
    const auto base = std::filesystem::canonical(root);
    const auto candidate = std::filesystem::weakly_canonical(base / path);
    auto child = candidate.begin();
    for (const auto& parent : base) {
        if (child == candidate.end() || _wcsicmp(parent.c_str(), child->c_str()) != 0)
            throw std::invalid_argument("Input escapes root");
        ++child;
    }
    return candidate;
}
}
