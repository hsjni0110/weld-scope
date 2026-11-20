#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "InspectionInput.h"
#include <opencv2/imgcodecs.hpp>
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace weldscope {
namespace {
using Failure = FileInspectionFailure;
constexpr std::size_t pngChunkOverhead = 12;

std::uint32_t read_be32(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void validate_png_end(const std::vector<unsigned char>& bytes)
{
    std::size_t offset = 8;
    while (bytes.size() - offset >= pngChunkOverhead) {
        const auto length = read_be32(bytes, offset);
        if (length > bytes.size() - offset - pngChunkOverhead) break;
        const auto next = offset + pngChunkOverhead + length;
        if (std::equal(bytes.begin() + offset + 4, bytes.begin() + offset + 8, "IEND")) {
            // A zero-length IEND has a fixed CRC over its four-byte type.
            if (length == 0 && next == bytes.size() && read_be32(bytes, offset + 8) == 0xae426082u) return;
            break;
        }
        offset = next;
    }
    throw InspectionInputError(Failure::DamagedPng, "Invalid PNG ending: expected one complete IEND at end of file");
}
}

std::vector<unsigned char> read_bytes(const std::filesystem::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw InspectionInputError(Failure::FileAccess, "File open failed: " + path.u8string());
    const auto size = input.tellg();
    if (size < 0) throw InspectionInputError(Failure::FileAccess, "Input length unavailable");
    if (static_cast<unsigned long long>(size) > limit)
        throw InspectionInputError(Failure::SizeLimit, "Input size limit exceeded");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    if (!input.seekg(0) || (size && !input.read(reinterpret_cast<char*>(bytes.data()), size)))
        throw InspectionInputError(Failure::FileAccess, "Input read failed");
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
    if (bytes.size() < sizeof(signature) || !std::equal(std::begin(signature), std::end(signature), bytes.begin()))
        throw InspectionInputError(Failure::UnsupportedFormat, "Unsupported image: expected PNG");
    if (bytes.size() < 33 || read_be32(bytes, 8) != 13
        || !std::equal(bytes.begin() + 12, bytes.begin() + 16, "IHDR"))
        throw InspectionInputError(Failure::DamagedPng, "Invalid PNG header: expected IHDR");
    const auto width = read_be32(bytes, 16), height = read_be32(bytes, 20);
    if (!width || !height) throw InspectionInputError(Failure::DamagedPng, "Invalid PNG dimensions");
    if (static_cast<std::uint64_t>(width) * height > 16000000)
        throw InspectionInputError(Failure::SizeLimit, "PNG pixel limit exceeded: 16000000");
    validate_png_end(bytes);
    auto image = cv::imdecode(bytes, cv::IMREAD_UNCHANGED);
    if (image.empty()) throw InspectionInputError(Failure::DamagedPng, "PNG decoding failed");
    return image;
}

int read_integer(const cv::FileNode& node, const char* name)
{
    if (!node.isInt()) throw std::invalid_argument(std::string("Missing or noninteger field: ") + name);
    const auto value = static_cast<std::int64_t>(node);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        throw std::invalid_argument(std::string("Integer field out of range: ") + name);
    return static_cast<int>(value);
}

InspectionSettings read_inspection_settings(const cv::FileStorage& json, const char* thresholdKey)
{
    if (read_integer(json["schema_version"], "schema_version") != 1)
        throw std::invalid_argument("Unsupported config schema");
    if (!json["fail_ratio"].isInt() && !json["fail_ratio"].isReal())
        throw std::invalid_argument("Numeric fail_ratio required");
    return {read_integer(json[thresholdKey], thresholdKey), static_cast<double>(json["fail_ratio"])};
}

BaselineConfig parse_baseline_config(const cv::FileStorage& json)
{
    const auto settings = read_inspection_settings(json, "intensity_threshold");
    return BaselineConfig(settings.threshold, settings.failRatio);
}

cv::Mat make_candidate_overlay(const cv::Mat& bgr, const cv::Mat& mask)
{
    cv::Mat colored = bgr.clone(), overlay;
    colored.setTo(cv::Scalar(0, 191, 255), mask);
    cv::addWeighted(bgr, 0.55, colored, 0.45, 0, overlay);
    return overlay;
}
}
