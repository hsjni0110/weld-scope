#pragma once

#include "AnalysisPipeline.h"
#include "InspectionInput.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace weldscope {
// 검증해 읽은 모델이 있으면 검사에 재사용한다. 독립 실행은 설정 경로에서 모델을 읽는다.
struct FileInspectionRequest {
    std::filesystem::path path;
    std::filesystem::path config;
    std::shared_ptr<const PooledSmoothModel> model = {};
};

// 판정, 근거와 화면 표시용 BGRA 원본 영상.
struct InspectedFile {
    std::filesystem::path path;
    std::string inputHash;
    cv::Mat original;
    InspectionOutcome outcome = InspectionOutcome::Error;
    std::optional<double> score;
    std::string reason;
    double analysisMs{};
};

// 검사 중 취소 요청을 확인했다. 파일 오류나 최종 판정과 구분하는 결과다.
struct InspectionCancelled {};
// 판정과 영상, 파일 처리 실패, 취소 중 하나를 전달한다.
using FileInspectionResult = std::variant<InspectedFile, FileInspectionFailure, InspectionCancelled>;

// 설정과 원본을 읽어 호출 스레드에서 한 장을 검사한다. 분석 사이에 취소 여부를 확인한다.
FileInspectionResult inspect_file(const FileInspectionRequest& request, const std::atomic_bool& cancelled);

// 저장할 원본과 설정의 지문. 처리 도중 실패하면 일부만 채워질 수 있다.
struct InspectionEvidence {
    std::string sourceSha256;
    std::string configSha256;
};
// 원본과 설정 지문을 기록하고, 장비가 지정한 원본과 바이트가 다른 경우 거부한다.
FileInspectionResult inspect_file_with_evidence(const FileInspectionRequest& request, const std::atomic_bool& cancelled,
    InspectionEvidence& evidence, std::string_view expectedSourceHash = {});

// 화면이 읽는 작업 상태. Cancelled나 Closing에서도 워커 회수가 남아 있을 수 있다.
enum class InspectionJobState { Empty, Ready, Running, Complete, Cancelled, Error, Closing };

// UI 스레드가 파일 선택과 검사 시작을 제어한다. 한 번에 워커 하나를 실행하며, UI가 poll로 회수한다.
class FileInspectionJob {
public:
    // 워커에서 호출하는 검사 함수. 받은 취소 플래그로 취소 요청을 확인한다.
    using Runner = std::function<FileInspectionResult(const FileInspectionRequest&, const std::atomic_bool&)>;
    // 검사 경로를 기억한다. 검사 중, 종료 중 또는 빈 경로면 false를 반환한다.
    bool select(const std::filesystem::path& path);
    // 선택한 파일의 검사 요청을 수락하면 true를 반환한다. 이전 워커를 먼저 회수해야 한다.
    bool try_start(const std::filesystem::path& config, Runner run = inspect_file);
    // 진행 중인 작업에 취소를 요청하고 늦게 도착한 결과를 무시한다. 워커 종료는 기다리지 않는다.
    void cancel();
    // 새 검사 시작을 막고 진행 중인 작업에 취소를 요청한다. poll로 남은 워커를 회수한다.
    void request_close();
    // 끝나지 않은 워커를 기다리지 않는다. 화면에서 받아들일 완료 결과가 없으면 std::nullopt를 반환한다.
    std::optional<FileInspectionResult> poll();

    // 화면 버튼과 상태를 갱신할 때 사용하는 작업 상태다.
    InspectionJobState state() const { return state_; }
    // 완료 여부와 관계없이 아직 poll로 회수할 워커가 있는지 알려 준다.
    bool busy() const { return worker_.valid(); }
    // 현재 선택 경로를 빌려 준다. 경로를 보존하려면 다음 선택 전에 복사한다.
    const std::filesystem::path& selected_path() const { return selected_; }

private:
    struct Completion {
        std::uint64_t id;
        FileInspectionResult result;
    };

    std::filesystem::path selected_;
    InspectionJobState state_ = InspectionJobState::Empty;
    std::uint64_t nextId_ = 0;
    std::optional<std::uint64_t> acceptedId_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::future<Completion> worker_;
};
}
