#pragma once

#include "filesearch/FileInfo.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace filesearch::analysis {

// 一组内容相同的文件
struct DuplicateGroup {
    std::uintmax_t file_size = 0;
    std::uint64_t content_hash = 0;
    std::vector<FileInfo> files;  // 至少 2 个，按路径排序

    // 每组只保留一份时可以释放的空间
    std::uintmax_t ReclaimableBytes() const {
        return file_size * (files.size() - 1);
    }
};

struct DuplicateReport {
    std::vector<DuplicateGroup> groups;  // 按可释放空间降序
    std::size_t candidate_file_count = 0;   // 按大小分组后需要哈希的文件数
    std::size_t unreadable_file_count = 0;  // 打不开或扫描后被修改的文件
    std::uintmax_t hashed_bytes = 0;        // 实际读取的字节数
};

struct HashProgress {
    std::size_t hashed_files = 0;
    std::size_t total_files = 0;
    std::uintmax_t hashed_bytes = 0;
    bool finished = false;
};

using HashProgressCallback = std::function<void(const HashProgress&)>;

// 三轮筛选：
//   1. 按大小分组（unordered_map），大小唯一的文件不可能重复，直接排除
//   2. 只读前 16 KB 计算快速哈希，再分组
//   3. 对仍然冲突的文件读全文计算 FNV-1a 64 位哈希，哈希相同即视为重复
// 第 2、3 轮用 std::async 把文件切片并行哈希
class DuplicateDetector {
public:
    explicit DuplicateDetector(std::size_t thread_count);

    void SetProgressCallback(HashProgressCallback progress_callback);

    DuplicateReport FindDuplicates(std::span<const FileInfo> files);

    // byte_limit 为 std::nullopt 时读全文。打开或读取失败、实际字节数与预期不符时返回 std::nullopt
    static std::optional<std::uint64_t> HashFileContent(const std::filesystem::path& path,
                                                        std::uintmax_t expected_size,
                                                        std::optional<std::uintmax_t> byte_limit);

private:
    // 对 candidates 中每个文件计算哈希，hashes[i] 对应 candidates[i]
    std::vector<std::optional<std::uint64_t>> HashInParallel(const std::vector<const FileInfo*>& candidates,
                                                             std::optional<std::uintmax_t> byte_limit);
    void HashWorker(const std::vector<const FileInfo*>& candidates, std::atomic<std::size_t>& next_candidate_index,
                    std::optional<std::uintmax_t> byte_limit, std::vector<std::optional<std::uint64_t>>& hashes);
    void ReportProgress(bool finished);

    std::size_t thread_count_;
    HashProgressCallback progress_callback_;
    std::size_t progress_total_files_ = 0;
    std::atomic<std::size_t> hashed_files_{0};
    std::atomic<std::uintmax_t> hashed_bytes_{0};
};

}  // namespace filesearch::analysis
