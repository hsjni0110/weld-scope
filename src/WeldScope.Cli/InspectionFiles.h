#pragma once
#include <filesystem>
#include <string>

namespace weldscope {
// 문자열 바이트를 지정 경로에 그대로 쓴다. 파일이 있으면 덮어쓴다.
void write_text(const std::filesystem::path& path, const std::string& text);
// 한 CSV 필드를 큰따옴표로 감싸고 내부 큰따옴표를 두 번 적는다.
std::string csv_field(const std::string& text);
// UTF-8 상대 경로를 확인해 root 안에서 접근할 경로를 반환한다. 상위 이동과 루트 밖 경로는 거부한다.
std::filesystem::path contained_path(const std::filesystem::path& root, const std::string& relative);
}
