#pragma once

#include "filesearch/FileInfo.hpp"

#include <cstdint>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace filesearch::filter {

// 用户给出的筛选条件；未设置的条件（空 vector / nullopt）表示不限制
struct FilterCriteria {
    std::vector<std::string> extensions;          // 小写、带点号，如 {".mp4", ".mkv"}
    std::optional<std::uintmax_t> minimum_size;   // 含边界
    std::optional<std::uintmax_t> maximum_size;   // 含边界
    std::optional<std::string> name_substring;    // 文件名包含该子串（ASCII 不区分大小写）
    std::optional<std::string> name_pattern;      // 文件名匹配该正则（ECMAScript，不区分大小写）
};

enum class SortKey {
    Size,  // 大小降序
    Path,  // 路径升序
    Time,  // 修改时间降序
};

class FileFilter {
public:
    // name_pattern 不是合法正则时抛出 std::regex_error
    explicit FileFilter(FilterCriteria criteria);

    bool Matches(const FileInfo& file) const;
    bool HasConditions() const;

    // 经典写法：std::copy_if + back_inserter
    std::vector<FileInfo> Apply(std::span<const FileInfo> files) const;
    // C++20 写法：views::filter 惰性视图 + ranges::copy 物化（程序默认使用这个版本）
    std::vector<FileInfo> ApplyWithRanges(std::span<const FileInfo> files) const;

    // std::ranges::sort + FileOrdering 中的具名比较器
    static void Sort(std::vector<FileInfo>& files, SortKey sort_key);

private:
    bool MatchesSize(std::uintmax_t size) const;
    bool MatchesExtension(std::string_view extension) const;
    bool MatchesName(const FileInfo& file) const;

    FilterCriteria criteria_;
    std::string lowered_substring_;
    // 用宽字符正则：Windows 下文件名本身就是 UTF-16，中文字符类（如 [一-龥]）才能按字符而不是按字节匹配
    std::optional<std::wregex> compiled_pattern_;
};

}  // namespace filesearch::filter
