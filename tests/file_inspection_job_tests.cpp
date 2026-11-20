#include "FileInspectionJob.h"
#include "InspectionInput.h"
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace weldscope;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const char* failure)
{
    if (!condition) throw std::runtime_error(failure);
}

std::optional<FileInspectionResult> drain(FileInspectionJob& job)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (job.busy() && std::chrono::steady_clock::now() < deadline) {
        if (auto result = job.poll()) return result;
        std::this_thread::yield();
    }
    check(!job.busy(), "Worker did not drain");
    return std::nullopt;
}

void check_lifecycle()
{
    FileInspectionJob job;
    check(job.select(L"A.png"), "Selection A rejected");
    std::promise<void> finish;
    auto gate = finish.get_future().share();
    check(job.start(L"baseline.json", [gate](const auto&, const auto&) -> FileInspectionResult {
        gate.wait();
        return InspectedFile{};
    }), "A did not start");
    const bool duplicateAccepted = job.start(L"baseline.json");
    job.cancel();
    const bool selected = job.select(L"B.png");
    const bool startedWhileDraining = job.start(L"baseline.json");
    finish.set_value();
    check(!duplicateAccepted && selected && !startedWhileDraining, "Single-slot admission failed");
    check(!drain(job) && job.state() == InspectionJobState::Ready, "Late A result was accepted");
    check(job.start(L"baseline.json", [](const auto& request, const auto&) -> FileInspectionResult {
        InspectedFile result;
        result.path = request.path;
        return result;
    }), "B did not start");
    const auto result = drain(job);
    check(result && std::get<InspectedFile>(*result).path == L"B.png"
          && job.state() == InspectionJobState::Complete, "B result was not retained");

    check(job.start(L"baseline.json", [](const auto&, const auto&) -> FileInspectionResult {
        return FileInspectionFailure::FileAccess;
    }), "Error-producing request did not start");
    job.cancel();
    check(job.select(L"C.png") && !drain(job) && job.state() == InspectionJobState::Ready,
          "Late error affected the replacement selection");

    check(job.start(L"baseline.json", [](const auto&, const auto&) -> FileInspectionResult {
        return FileInspectionFailure::FileAccess;
    }), "Retry did not start");
    job.request_close();
    check(!drain(job) && job.state() == InspectionJobState::Closing, "Close accepted a late error");
    check(!job.select(L"C.png") && !job.start(L"baseline.json"), "Closing reopened admission");
}

void check_input(const std::filesystem::path& fixture, const std::filesystem::path& config)
{
    const std::atomic_bool cancelled{false};
    const auto result = inspect_file({fixture, config}, cancelled);
    const auto* image = std::get_if<InspectedFile>(&result);
    check(image && image->outcome == InspectionOutcome::Fail && image->original.cols == 414
          && image->original.rows == 40, "Real PNG inspection mismatch");
    check(image->inputHash == "4a08fa2c6274e603f18358134092e8f697cc81f78a36f472577bca81bc0de3f3"
          && image->original.type() == CV_8UC4 && !image->overlay.empty(), "Result provenance or buffer mismatch");
    check(image->score == 16047.0 / 16560.0, "Score differs from S04");
    check(image->attempts.size() == 1 && image->attempts.front().stage == "BASELINE",
          "Baseline stage was not retained");
    const cv::Mat bgr(1, 2, CV_8UC3, cv::Scalar(100, 100, 100));
    const cv::Mat mask = (cv::Mat_<unsigned char>(1, 2) << 0, 255);
    const auto overlay = make_candidate_overlay(bgr, mask);
    check(overlay.at<cv::Vec3b>(0, 0) == cv::Vec3b(100, 100, 100)
          && overlay.at<cv::Vec3b>(0, 1) == cv::Vec3b(55, 141, 170), "Overlay does not follow the mask");
    const std::atomic_bool stopped{true};
    check(std::holds_alternative<InspectionCancelled>(inspect_file({fixture, config}, stopped)),
          "Cancelled file request was inspected");
    const auto missing = fixture.parent_path() / L"not-an-inspection-fixture.png";
    check(std::get<FileInspectionFailure>(inspect_file({missing, config}, cancelled)) == FileInspectionFailure::FileAccess,
          "Missing file was not classified as an access error");
    check(std::get<FileInspectionFailure>(inspect_file({fixture, missing}, cancelled)) == FileInspectionFailure::Configuration,
          "Missing configuration was classified as an input error");

    const auto json = read_json({'{','"','s','c','h','e','m','a','_','v','e','r','s','i','o','n','"',':','2','}'});
    try {
        parse_baseline_config(json);
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid configuration schema accepted");
}

void check_selective_cancellation(const std::filesystem::path& fixture, const std::filesystem::path& config)
{
    const std::string settings =
        R"({"schema_version":1,"contrast_threshold":12,"fast_pass_max":0,"fast_fail_min":1,)"
        R"("precise_pass_max":0,"precise_fail_min":1,"budget_us":1000000})";
    const std::vector<unsigned char> bytes(settings.begin(), settings.end());
    std::atomic_bool cancelled{false};
    InspectionEvidence evidence;
    bool notified = false;
    const auto result = inspect_file_with_evidence({fixture, config, InspectionMode::Selective}, cancelled,
        evidence, &bytes, {}, [&] { notified = true; cancelled = true; });
    check(notified && std::holds_alternative<InspectionCancelled>(result),
          "Cancel at the selective stage boundary did not discard the verdict");
    cancelled = false;
    const auto completed = inspect_file_with_evidence({fixture, config, InspectionMode::Selective}, cancelled,
        evidence, &bytes);
    const auto* image = std::get_if<InspectedFile>(&completed);
    check(image && image->outcome == InspectionOutcome::Review && image->attempts.size() == 2 &&
          image->attempts[0].stage == "FAST" && image->attempts[1].stage == "PRECISE",
          "Selective inspection did not retain both attempted stages");
}

void check_input_errors(const std::filesystem::path& fixture)
{
    const auto rejects = [](auto operation, FileInspectionFailure expected) {
        try {
            operation();
        } catch (const InspectionInputError& error) {
            check(error.reason() == expected, "Wrong file error category");
            return;
        }
        throw std::runtime_error("Invalid input was accepted");
    };
    rejects([&] { read_bytes(fixture, 1); }, FileInspectionFailure::SizeLimit);
    rejects([] { decode_png({'P', 'N', 'G'}); }, FileInspectionFailure::UnsupportedFormat);
    auto damaged = read_bytes(fixture, image_file_limit);
    damaged.back() ^= 1;
    rejects([&] { decode_png(damaged); }, FileInspectionFailure::DamagedPng);
}
}

int wmain(int argc, wchar_t** argv)
{
    try {
        check(argc == 3, "Expected fixture and config paths");
        check_lifecycle();
        check_input(argv[1], argv[2]);
        check_selective_cancellation(argv[1], argv[2]);
        check_input_errors(argv[1]);
        std::cout << "File inspection job checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
