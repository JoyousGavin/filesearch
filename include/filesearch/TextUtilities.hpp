#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace filesearch::text {

// 只转换 ASCII 字母，UTF-8 多字节序列原样保留
std::string ToLowerAscii(std::string_view text);

// path <-> UTF-8 字符串。统一走 u8string，避免 MSVC 的 path::string() 按 ANSI 代码页转换时抛异常
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(std::string_view utf8_text);

}  // namespace filesearch::text
