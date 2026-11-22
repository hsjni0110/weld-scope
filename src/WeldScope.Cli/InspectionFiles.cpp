#include "InspectionFiles.h"
#include <fstream>
#include <stdexcept>

namespace weldscope {
void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
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
