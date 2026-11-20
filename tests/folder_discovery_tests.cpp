#include "IntegratedInspection.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("weldscope-folder-discovery-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    Fixture() { fs::create_directories(root / "a"); fs::create_directories(root / "b"); }
    ~Fixture() { std::error_code ignored; fs::remove_all(root, ignored); }
};
}

int main() {
    try {
        Fixture fixture;
        std::ofstream(fixture.root / "b/image.PNG").put('\0');
        std::ofstream(fixture.root / "a/image.png").put('\0');
        std::ofstream(fixture.root / "other.jpg").put('\0');
        fs::create_directory(fixture.root / "folder.png");
        std::error_code noSymlinkPrivilege;
        fs::create_directory_symlink(fixture.root / "a", fixture.root / "linked", noSymlinkPrivilege);

        const std::vector<fs::path> expected{"a/image.png", "b/image.PNG"};
        Check(weldscope::FindInspectionFiles(fixture.root) == expected, "expected sorted relative PNG paths, through real directories");
        Check(weldscope::FindInspectionFiles(fixture.root / "folder.png").empty(), "an empty folder yields no inputs");
        bool missingReported = false;
        try { (void)weldscope::FindInspectionFiles(fixture.root / "missing"); }
        catch (const fs::filesystem_error&) { missingReported = true; }
        Check(missingReported, "a missing directory must be reported");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
