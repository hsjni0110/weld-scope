#include "FileInspectionJob.h"
#include "InspectionInput.h"
#include <chrono>
#include <cmath>
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
    check(job.try_start(L"pooled-smooth-220.json", [gate](const auto&, const auto&) -> FileInspectionResult {
        gate.wait();
        return InspectedFile{};
    }), "A did not start");
    const bool duplicateAccepted = job.try_start(L"pooled-smooth-220.json");
    job.cancel();
    const bool selected = job.select(L"B.png");
    const bool startedWhileDraining = job.try_start(L"pooled-smooth-220.json");
    finish.set_value();
    check(!duplicateAccepted && selected && !startedWhileDraining, "Single-slot admission failed");
    check(!drain(job) && job.state() == InspectionJobState::Ready, "Late A result was accepted");
    check(job.try_start(L"pooled-smooth-220.json", [](const auto& request, const auto&) -> FileInspectionResult {
        InspectedFile result;
        result.path = request.path;
        return result;
    }), "B did not start");
    const auto result = drain(job);
    check(result && std::get<InspectedFile>(*result).path == L"B.png"
          && job.state() == InspectionJobState::Complete, "B result was not retained");

    check(job.try_start(L"pooled-smooth-220.json", [](const auto&, const auto&) -> FileInspectionResult {
        return FileInspectionFailure::FileAccess;
    }), "Error-producing request did not start");
    job.cancel();
    check(job.select(L"C.png") && !drain(job) && job.state() == InspectionJobState::Ready,
          "Late error affected the replacement selection");

    check(job.try_start(L"pooled-smooth-220.json", [](const auto&, const auto&) -> FileInspectionResult {
        return FileInspectionFailure::FileAccess;
    }), "Retry did not start");
    job.request_close();
    check(!drain(job) && job.state() == InspectionJobState::Closing, "Close accepted a late error");
    check(!job.select(L"C.png") && !job.try_start(L"pooled-smooth-220.json"), "Closing reopened admission");
}

void check_input(const std::filesystem::path& fixture, const std::filesystem::path& config)
{
    const std::atomic_bool cancelled{false};
    const auto result = inspect_file({fixture, config}, cancelled);
    const auto* image = std::get_if<InspectedFile>(&result);
    check(image && image->outcome == InspectionOutcome::Fail && image->original.cols == 414
          && image->original.rows == 40, "Real PNG inspection mismatch");
    check(image->inputHash == "4a08fa2c6274e603f18358134092e8f697cc81f78a36f472577bca81bc0de3f3"
          && image->original.type() == CV_8UC4, "Result provenance or buffer mismatch");
    check(image->score && std::abs(*image->score - 0.7688432614002394) < 1e-12 &&
          image->reason == "score_threshold" && image->analysisMs >= 0, "LightGBM result differs");
    const std::atomic_bool stopped{true};
    check(std::holds_alternative<InspectionCancelled>(inspect_file({fixture, config}, stopped)),
          "Cancelled file request was inspected");
    const auto missing = fixture.parent_path() / L"not-an-inspection-fixture.png";
    check(std::get<FileInspectionFailure>(inspect_file({missing, config}, cancelled)) == FileInspectionFailure::FileAccess,
          "Missing file was not classified as an access error");
    check(std::get<FileInspectionFailure>(inspect_file({fixture, missing}, cancelled)) == FileInspectionFailure::Configuration,
          "Missing configuration was classified as an input error");

    InspectionEvidence evidence;
    check(std::get<FileInspectionFailure>(inspect_file_with_evidence({fixture, config}, cancelled, evidence,
        "wrong-image-hash")) == FileInspectionFailure::FileAccess, "Device source hash mismatch accepted");
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
        check_input_errors(argv[1]);
        std::cout << "File inspection job checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
