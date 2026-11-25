#include "filesearch/FileAnalyzer.hpp"

#include <algorithm>
#include <functional>
#include <numeric>
#include <queue>
#include <ranges>
#include <unordered_map>
#include <utility>

namespace filesearch::analysis {

namespace {

// reference_wrapper 可以隐式转换为 const FileInfo&，所以 FileOrdering 的比较函数能直接用于它，
// 堆和 nth_element 操作的是引用而不是整份 FileInfo（不复制路径字符串）
using FileReference = std::reference_wrapper<const FileInfo>;
using FileComparator = bool (*)(const FileInfo&, const FileInfo&);

struct ExtensionTotals {
    std::size_t file_count = 0;
    std::uintmax_t total_bytes = 0;
};

}  // namespace

std::string_view ToString(TopAlgorithm top_algorithm) {
    switch (top_algorithm) {
        case TopAlgorithm::Heap:
            return "heap (priority_queue)";
        case TopAlgorithm::PartialSortCopy:
            return "partial_sort_copy";
        case TopAlgorithm::NthElement:
            return "nth_element + sort";
        case TopAlgorithm::Ranges:
            return "ranges::partial_sort_copy";
        case TopAlgorithm::FullSort:
            return "full sort";
    }
    return "unknown";
}

std::vector<ExtensionStatistics> FileAnalyzer::ComputeExtensionStatistics(std::span<const FileInfo> files) {
    std::unordered_map<std::string, ExtensionTotals> totals_by_extension;
    totals_by_extension.reserve(256);  // 预留桶，避免统计过程中反复 rehash
    for (const FileInfo& file : files) {
        ExtensionTotals& totals = totals_by_extension[file.extension];  // 不存在时 operator[] 会值初始化一个新条目
        ++totals.file_count;
        totals.total_bytes += file.size;
    }

    std::vector<ExtensionStatistics> statistics;
    statistics.reserve(totals_by_extension.size());
    for (auto& [extension, totals] : totals_by_extension) {
        statistics.push_back(ExtensionStatistics{extension, totals.file_count, totals.total_bytes});
    }
    // ranges 投影：先按后缀名升序，再按总大小降序做稳定排序 -> 同大小的后缀保持字母序，输出可复现
    std::ranges::sort(statistics, std::ranges::less{}, &ExtensionStatistics::extension);  //call std::ranges::less，比较投影 ExtensionStatistics::extension
    std::ranges::stable_sort(statistics, std::ranges::greater{}, &ExtensionStatistics::total_bytes);  //call std::ranges::greater，比较投影 ExtensionStatistics::total_bytes
    return statistics;
}

std::uintmax_t FileAnalyzer::SumBytes(std::span<const FileInfo> files) {
    // views::transform 配合数据成员指针投影出 size 列，再交给 std::accumulate
    auto sizes = files | std::views::transform(&FileInfo::size);  //call std::invoke(&FileInfo::size, file)，迭代时逐个取成员
    return std::accumulate(sizes.begin(), sizes.end(), std::uintmax_t{0});
}

std::vector<FileInfo> FileAnalyzer::TopLargest(std::span<const FileInfo> files, std::size_t top_count,
                                               TopAlgorithm top_algorithm) {
    switch (top_algorithm) {
        case TopAlgorithm::Heap:
            return TopLargestWithHeap(files, top_count);
        case TopAlgorithm::PartialSortCopy:
            return TopLargestWithPartialSortCopy(files, top_count);
        case TopAlgorithm::NthElement:
            return TopLargestWithNthElement(files, top_count);
        case TopAlgorithm::Ranges:
            return TopLargestWithRanges(files, top_count);
        case TopAlgorithm::FullSort:
            return TopLargestWithFullSort(files, top_count);
    }
    return {};
}

std::vector<FileInfo> FileAnalyzer::TopLargestWithHeap(std::span<const FileInfo> files, std::size_t top_count) {
    if (top_count == 0) {
        return {};
    }
    // priority_queue 把比较器意义下“最大”的元素放在堆顶。比较器取“大小降序”时，
    // 排在最后的（最小的）文件被视为最大 -> 堆顶始终是当前 K 个里最小的那个，即小顶堆
    std::priority_queue<FileReference, std::vector<FileReference>, FileComparator> minimum_heap(
        &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending（push/pop 调整堆时调用）

    for (const FileInfo& file : files) {
        if (minimum_heap.size() < top_count) {
            minimum_heap.push(std::cref(file));
        } else if (FileOrdering::BySizeDescending(file, minimum_heap.top())) {
            // 新文件比堆里最小的还大：踢掉堆顶，换它进来
            minimum_heap.pop();
            minimum_heap.push(std::cref(file));
        }
    }

    std::vector<FileInfo> top_files;
    top_files.reserve(minimum_heap.size());
    while (!minimum_heap.empty()) {
        top_files.push_back(minimum_heap.top().get());
        minimum_heap.pop();
    }
    std::reverse(top_files.begin(), top_files.end());  // 小顶堆依次弹出的是从小到大，反转成从大到小
    return top_files;
}

std::vector<FileInfo> FileAnalyzer::TopLargestWithPartialSortCopy(std::span<const FileInfo> files,
                                                                  std::size_t top_count) {
    std::vector<FileInfo> top_files(std::min(top_count, files.size()));
    std::partial_sort_copy(files.begin(), files.end(), top_files.begin(), top_files.end(),
                           &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending
    return top_files;
}

std::vector<FileInfo> FileAnalyzer::TopLargestWithNthElement(std::span<const FileInfo> files, std::size_t top_count) {
    std::vector<FileReference> references(files.begin(), files.end());
    const auto boundary = references.begin() + static_cast<std::ptrdiff_t>(std::min(top_count, references.size()));
    // nth_element 之后，boundary 之前的元素都不“小于” boundary 处的元素（但彼此无序），再只对这一段排序
    std::nth_element(references.begin(), boundary, references.end(),
                     &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending
    std::sort(references.begin(), boundary, &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending
    return std::vector<FileInfo>(references.begin(), boundary);
}

std::vector<FileInfo> FileAnalyzer::TopLargestWithRanges(std::span<const FileInfo> files, std::size_t top_count) {
    std::vector<FileInfo> top_files(std::min(top_count, files.size()));
    // 范围版本直接接收整个容器；返回的 partial_sort_copy_result 中 .in / .out 分别指向输入、输出处理到的位置
    const auto copy_result =
        std::ranges::partial_sort_copy(files, top_files, &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending
    top_files.erase(copy_result.out, top_files.end());
    return top_files;
}

std::vector<FileInfo> FileAnalyzer::TopLargestWithFullSort(std::span<const FileInfo> files, std::size_t top_count) {
    std::vector<FileInfo> sorted_files(files.begin(), files.end());
    std::sort(sorted_files.begin(), sorted_files.end(), &FileOrdering::BySizeDescending);  //call filesearch::FileOrdering::BySizeDescending
    sorted_files.resize(std::min(top_count, sorted_files.size()));
    return sorted_files;
}

}  // namespace filesearch::analysis
