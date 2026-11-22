#include "InspectionDetails.h"

#include "InspectionInput.h"

#include <filesystem>
#include <opencv2/imgproc.hpp>
#include <stdexcept>
#include <utility>

namespace weldscope {
OpenedInspection ReadInspectionDetails(const std::string& inspectionId)
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
