#include "filesearch/FileFilter.hpp"

#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace filesearch::filter {

FileFilter::FileFilter(FilterCriteria criteria) : criteria_(std::move(criteria)) {
    if (criteria_.name_substring) {
        lowered_substring_ = text::ToLowerAscii(*criteria_.name_substring);
    }
    if (criteria_.name_pattern) {
        // optimize：构造时多花时间编译成更快的匹配器，适合“编译一次、匹配很多次”的场景
        compiled_pattern_.emplace(text::Utf8ToWide(*criteria_.name_pattern),
                                  std::regex_constants::ECMAScript | std::regex_constants::icase |
                                      std::regex_constants::optimize);
    }
}

bool FileFilter::HasConditions() const {
    return !criteria_.extensions.empty() || criteria_.minimum_size || criteria_.maximum_size ||
           criteria_.name_substring || criteria_.name_pattern;
}

bool FileFilter::Matches(const FileInfo& file) const {
    // 先做最便宜的判断，正则放在最后
    return MatchesSize(file.size) && MatchesExtension(file.extension) && MatchesName(file);
}

bool FileFilter::MatchesSize(std::uintmax_t size) const {
    if (criteria_.minimum_size && size < *criteria_.minimum_size) {
        return false;
    }
    return !(criteria_.maximum_size && size > *criteria_.maximum_size);
}

bool FileFilter::MatchesExtension(std::string_view extension) const {
    if (criteria_.extensions.empty()) {
        return true;
    }
    return std::find(criteria_.extensions.begin(), criteria_.extensions.end(), extension) !=
           criteria_.extensions.end();
}

bool FileFilter::MatchesName(const FileInfo& file) const {
    if (!criteria_.name_substring && !compiled_pattern_) {
        return true;
    }
    const std::filesystem::path file_name = file.path.filename();
    if (criteria_.name_substring) {
        const std::string lowered_name = text::ToLowerAscii(text::PathToUtf8(file_name));
        if (lowered_name.find(lowered_substring_) == std::string::npos) {
            return false;
        }
    }
    if (compiled_pattern_) {
        // regex_search：文件名中任意位置出现匹配即可；需要整名匹配时在模式里写 ^...$
        return std::regex_search(file_name.wstring(), *compiled_pattern_);
    }
    return true;
}

std::vector<FileInfo> FileFilter::Apply(std::span<const FileInfo> files) const {
    std::vector<FileInfo> matched_files;
    std::copy_if(files.begin(), files.end(), std::back_inserter(matched_files),
                 [this](const FileInfo& file) {
                     return Matches(file);  //call filesearch::filter::FileFilter::Matches
                 });  //call 上面的 lambda（std::copy_if 对每个元素调用一次）
    return matched_files;
}

std::vector<FileInfo> FileFilter::ApplyWithRanges(std::span<const FileInfo> files) const {
    // views::filter 不复制任何元素，只在迭代时调用谓词；ranges::copy 遍历视图时才真正拷贝匹配项
    auto matched_view = files | std::views::filter([this](const FileInfo& file) {
                            return Matches(file);  //call filesearch::filter::FileFilter::Matches
                        });  //call 上面的 lambda（filter_view 迭代时逐个调用）
    std::vector<FileInfo> matched_files;
    std::ranges::copy(matched_view, std::back_inserter(matched_files));
    return matched_files;
}

void FileFilter::Sort(std::vector<FileInfo>& files, SortKey sort_key) {
    switch (sort_key) {
        case SortKey::Size:
            std::ranges::sort(files, &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending
            break;
        case SortKey::Path:
            std::ranges::sort(files, &FileOrdering::ByPathAscending);  //call filesearch::FileOrdering::ByPathAscending
            break;
        case SortKey::Time:
            std::ranges::sort(files, &FileOrdering::ByTimeDescending);  //call filesearch::FileOrdering::ByTimeDescending
            break;
    }
}

}  // namespace filesearch::filter
