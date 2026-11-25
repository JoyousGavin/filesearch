#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace filesearch::text {

// 只转换 ASCII 字母，UTF-8 多字节序列原样保留
std::string ToLowerAscii(std::string_view text);

// 去掉首尾空白；返回的 string_view 指向原字符串，不发生拷贝
std::string_view TrimWhitespace(std::string_view text);

// 按分隔符切分；返回的各段 string_view 都指向原字符串，调用方要保证原字符串存活
std::vector<std::string_view> SplitString(std::string_view text, char delimiter);

// UTF-8 -> 宽字符串（Windows 下为 UTF-16），借助 filesystem::path 的编码转换实现
std::wstring Utf8ToWide(std::string_view utf8_text);

// C++20 透明哈希：让 unordered_map<std::string, ...> 能直接用 string_view / 字面量查找，查找时不构造临时 std::string
struct TransparentStringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view text) const noexcept {
        return std::hash<std::string_view>{}(text);
    }
};

// path <-> UTF-8 字符串。统一走 u8string，避免 MSVC 的 path::string() 按 ANSI 代码页转换时抛异常
std::string PathToUtf8(const std::filesystem::path& path);
std::filesystem::path PathFromUtf8(std::string_view utf8_text);

}  // namespace filesearch::text
