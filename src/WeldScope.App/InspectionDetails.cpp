#include "InspectionDetails.h"

#include "AnalysisPipeline.h"
#include "InspectionInput.h"
#include "LocalContrastInspection.h"

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <utility>

namespace weldscope {
namespace {

cv::Mat RebuildSavedCandidateOverlay(const cv::Mat& bgr, const InspectionRecord& record, const std::filesystem::path& baseline)
{
    const auto attempt = std::find_if(record.attempts.rbegin(), record.attempts.rend(),
                                      [](const AnalysisAttempt& stage) { return stage.score.has_value(); });
    if (attempt == record.attempts.rend())
        return {};

    // 영상 전체의 점수만 내는 단계는 픽셀별 후보를 만들지 않는다.
    if (attempt->stage == "POOLED_WIDTH" || attempt->stage == "POOLED" || attempt->stage == "SPATIAL")
        return {};
    const bool isBaseline = record.algorithmVersion == inspection_version(InspectionMode::Baseline);
    auto path = baseline;
    if (!isBaseline) {
        struct SavedConfig {
            const char* version;
            const wchar_t* filename;
        };
        const SavedConfig configs[] = {
            {inspection_version(InspectionMode::Selective), L"selective.json"},
            {inspection_version(InspectionMode::PooledSmooth), pooledSmoothConfigFile},
            {"openzdm-spatial-48-v1", L"spatial-48.json"},
            {"openzdm-pooled-400-v1", L"pooled-400.json"},
            {"openzdm-pooled-width-220-v1", L"pooled-width-220.json"}};
        const auto saved = std::find_if(std::begin(configs), std::end(configs),
                                        [&](const SavedConfig& entry) { return record.algorithmVersion == entry.version; });
        if (saved == std::end(configs))
            throw std::runtime_error("The saved algorithm version is unavailable");
        path = baseline.parent_path() / saved->filename;
    }
    const auto config = read_bytes(path, config_file_limit);
    if (sha256(config) != record.configSha256)
        throw std::runtime_error("The saved settings are unavailable");

    cv::Mat mask;
    if (isBaseline) {
        mask = inspect_baseline(bgr, parse_baseline_config(read_json(config))).mask;
    } else {
        const bool precise = attempt->stage == "PRECISE";
        const auto settings = parse_inspection_parameters(read_json(config), InspectionMode::Selective);
        mask = inspect_local_contrast(bgr, precise ? LocalContrastMode::Precise : LocalContrastMode::Fast,
                                      settings.threshold, precise ? settings.preciseBand.failMin : settings.fastBand.failMin)
                   .mask;
    }
    if (!cv::countNonZero(mask))
        return {};
    cv::Mat overlay;
    cv::cvtColor(make_candidate_overlay(bgr, mask), overlay, cv::COLOR_BGR2BGRA);
    return overlay;
}

} // namespace

OpenedInspection ReadInspectionDetails(const std::string& inspectionId, InspectionDetailSource source)
{
    auto stored = InspectionHistory(HistoryConnectionFromEnvironment()).Get(inspectionId);
    if (!stored)
        throw std::runtime_error("Inspection history row unavailable");

    OpenedInspection opened;
    opened.record = std::move(stored->record);
    const auto& record = opened.record;
    try {
        const auto bytes = read_bytes(std::filesystem::u8path(record.sourcePath), image_file_limit);
        if (record.sourceSha256.empty()) {
            opened.sourceStatus = InspectionSourceStatus::Unverifiable;
        } else if (sha256(bytes) != record.sourceSha256) {
            opened.sourceStatus = InspectionSourceStatus::Changed;
        } else {
            const auto bgr = decode_png(bytes);
            cv::cvtColor(bgr, opened.image, cv::COLOR_BGR2BGRA);
            if (source == InspectionDetailSource::FolderResult) {
                try {
                    opened.overlay = RebuildSavedCandidateOverlay(bgr, record, baseline_config_path());
                } catch (const std::exception&) {
                    opened.sourceStatus = InspectionSourceStatus::OverlaySettingsUnavailable;
                }
            }
        }
    } catch (const InspectionInputError& error) {
        opened.sourceStatus = error.reason() == FileInspectionFailure::FileAccess
                                  ? InspectionSourceStatus::Missing
                                  : InspectionSourceStatus::Unreadable;
    } catch (const std::exception&) {
        opened.sourceStatus = InspectionSourceStatus::Unreadable;
    }
    return opened;
}

} // namespace weldscope
