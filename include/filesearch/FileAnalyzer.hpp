#pragma once

#include "filesearch/FileInfo.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace filesearch::analysis {

// 某个后缀名的空间占用汇总
struct ExtensionStatistics {
    std::string extension;  // 空字符串表示“无后缀”
    std::size_t file_count = 0;
    std::uintmax_t total_bytes = 0;
};

// Top K 的几种实现，复杂度对比：
//   Heap            std::priority_queue 小顶堆，只保留 K 个元素       O(N log K)，额外内存 O(K)
//   PartialSortCopy std::partial_sort_copy 直接拷出前 K 个            O(N log K)，额外内存 O(K)
//   NthElement      std::nth_element 选出前 K 个再对这 K 个排序       平均 O(N + K log K)，额外内存 O(N) 个引用
//   FullSort        整体拷贝后 std::sort，作为对照基准                O(N log N)，额外内存 O(N)
enum class TopAlgorithm {
    Heap,
    PartialSortCopy,
    NthElement,
    FullSort,
};

std::string_view ToString(TopAlgorithm top_algorithm);

class FileAnalyzer {
public:
    // unordered_map<后缀, 汇总> 累加，再转成 vector 按占用空间降序
    static std::vector<ExtensionStatistics> ComputeExtensionStatistics(std::span<const FileInfo> files);

    static std::uintmax_t SumBytes(std::span<const FileInfo> files);

    // 返回最大的 top_count 个文件（大小降序），结果是拷贝，不依赖输入的生命周期
    static std::vector<FileInfo> TopLargest(std::span<const FileInfo> files, std::size_t top_count,
                                            TopAlgorithm top_algorithm);

    static std::vector<FileInfo> TopLargestWithHeap(std::span<const FileInfo> files, std::size_t top_count);
    static std::vector<FileInfo> TopLargestWithPartialSortCopy(std::span<const FileInfo> files,
                                                               std::size_t top_count);
    static std::vector<FileInfo> TopLargestWithNthElement(std::span<const FileInfo> files, std::size_t top_count);
    static std::vector<FileInfo> TopLargestWithFullSort(std::span<const FileInfo> files, std::size_t top_count);
};

}  // namespace filesearch::analysis
