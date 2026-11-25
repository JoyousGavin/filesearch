#include "filesearch/DuplicateDetector.hpp"

#include <algorithm>
#include <fstream>
#include <future>
#include <ios>
#include <string>
#include <unordered_map>
#include <utility>

namespace filesearch::analysis {

namespace {

constexpr std::uintmax_t kQuickHashBytes = 16 * 1024;
constexpr std::size_t kReadBufferBytes = 64 * 1024;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::chrono::milliseconds kProgressInterval{100};

std::uint64_t UpdateFnv1a(std::uint64_t hash, std::span<const char> bytes) {
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= kFnvPrime;
    }
    return hash;
}

// 分组键：(大小, 哈希)
struct SizeAndHash {
    std::uintmax_t file_size = 0;
    std::uint64_t content_hash = 0;

    bool operator==(const SizeAndHash&) const = default;  // C++20 默认比较
};

struct SizeAndHashHasher {
    std::size_t operator()(const SizeAndHash& key) const noexcept {
        // 与 boost::hash_combine 相同的混合方式
        std::size_t seed = std::hash<std::uintmax_t>{}(key.file_size);
        seed ^= std::hash<std::uint64_t>{}(key.content_hash) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
        return seed;
    }
};

using CandidateGroups = std::unordered_map<SizeAndHash, std::vector<const FileInfo*>, SizeAndHashHasher>;

bool CompareGroupsByReclaimableBytes(const DuplicateGroup& left, const DuplicateGroup& right) {
    if (left.ReclaimableBytes() != right.ReclaimableBytes()) {
        return left.ReclaimableBytes() > right.ReclaimableBytes();
    }
    return left.files.front().path.native() < right.files.front().path.native();
}

}  // namespace

DuplicateDetector::DuplicateDetector(std::size_t thread_count) : thread_count_(std::max<std::size_t>(1, thread_count)) {}

void DuplicateDetector::SetProgressCallback(HashProgressCallback progress_callback) {
    progress_callback_ = std::move(progress_callback);
}

std::optional<std::uint64_t> DuplicateDetector::HashFileContent(const std::filesystem::path& path,
                                                               std::uintmax_t expected_size,
                                                               std::optional<std::uintmax_t> byte_limit) {
    std::ifstream input(path, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return std::nullopt;
    }

    // thread_local：每个哈希线程只分配一次读缓冲区，之后的文件复用
    thread_local std::vector<char> buffer(kReadBufferBytes);
    const std::uintmax_t bytes_wanted = byte_limit ? std::min(*byte_limit, expected_size) : expected_size;
    std::uint64_t hash = kFnvOffsetBasis;
    std::uintmax_t bytes_total = 0;
    while (bytes_total < bytes_wanted) {
        const std::uintmax_t bytes_remaining = bytes_wanted - bytes_total;
        const auto bytes_requested =
            static_cast<std::streamsize>(std::min<std::uintmax_t>(bytes_remaining, buffer.size()));
        input.read(buffer.data(), bytes_requested);
        const std::streamsize bytes_read = input.gcount();  // 最后一块可能不满
        if (bytes_read <= 0) {
            break;
        }
        hash = UpdateFnv1a(hash, std::span<const char>(buffer.data(), static_cast<std::size_t>(bytes_read)));
        bytes_total += static_cast<std::uintmax_t>(bytes_read);
    }

    if (bytes_total != bytes_wanted) {
        return std::nullopt;  // 扫描之后文件被截断，或读取出错
    }
    if (!byte_limit && input.peek() != std::ifstream::traits_type::eof()) {
        return std::nullopt;  // 读全文时还能读到数据，说明扫描之后文件变长了
    }
    return hash;
}

void DuplicateDetector::HashWorker(const std::vector<const FileInfo*>& candidates,
                                   std::atomic<std::size_t>& next_candidate_index,
                                   std::optional<std::uintmax_t> byte_limit,
                                   std::vector<std::optional<std::uint64_t>>& hashes) {
    // 原子下标动态领取任务：大文件不会集中堆在某一个线程上
    for (std::size_t index = next_candidate_index.fetch_add(1); index < candidates.size();
         index = next_candidate_index.fetch_add(1)) {
        const FileInfo& file = *candidates[index];
        hashes[index] = HashFileContent(file.path, file.size, byte_limit);  // 各线程写不同下标，无需加锁
        hashed_files_.fetch_add(1, std::memory_order_relaxed);
        if (hashes[index]) {
            hashed_bytes_.fetch_add(byte_limit ? std::min(*byte_limit, file.size) : file.size,
                                    std::memory_order_relaxed);
        }
    }
}

std::vector<std::optional<std::uint64_t>> DuplicateDetector::HashInParallel(
    const std::vector<const FileInfo*>& candidates, std::optional<std::uintmax_t> byte_limit) {
    std::vector<std::optional<std::uint64_t>> hashes(candidates.size());
    std::atomic<std::size_t> next_candidate_index{0};
    const std::size_t task_count = std::min(thread_count_, candidates.size());

    std::vector<std::future<void>> futures;
    futures.reserve(task_count);
    for (std::size_t task_index = 0; task_index < task_count; ++task_index) {
        futures.push_back(std::async(std::launch::async, &DuplicateDetector::HashWorker, this, std::cref(candidates),
                                     std::ref(next_candidate_index), byte_limit,
                                     std::ref(hashes)));  //call filesearch::analysis::DuplicateDetector::HashWorker
    }

    // 主线程不闲着：wait_for 超时就汇报一次进度，直到任务完成
    for (std::future<void>& future : futures) {
        while (future.wait_for(kProgressInterval) != std::future_status::ready) {
            ReportProgress(false);
        }
    }
    for (std::future<void>& future : futures) {
        future.get();  // 任务中抛出的异常在这里重新抛出
    }
    return hashes;
}

void DuplicateDetector::ReportProgress(bool finished) {
    if (!progress_callback_) {
        return;
    }
    HashProgress progress;
    progress.hashed_files = hashed_files_.load(std::memory_order_relaxed);
    progress.total_files = progress_total_files_;
    progress.hashed_bytes = hashed_bytes_.load(std::memory_order_relaxed);
    progress.finished = finished;
    progress_callback_(progress);  //call filesearch::render::ConsoleRenderer::RenderHashProgress（由 Application 通过 SetProgressCallback 注册）
}

DuplicateReport DuplicateDetector::FindDuplicates(std::span<const FileInfo> files) {
    DuplicateReport report;
    hashed_files_.store(0);
    hashed_bytes_.store(0);

    // 第 1 轮：按大小分组。空文件彼此“内容相同”没有清理价值，跳过
    std::unordered_map<std::uintmax_t, std::vector<const FileInfo*>> files_by_size;
    for (const FileInfo& file : files) {
        if (file.size > 0) {
            files_by_size[file.size].push_back(&file);
        }
    }
    std::vector<const FileInfo*> size_candidates;
    for (const auto& [file_size, members] : files_by_size) {
        if (members.size() > 1) {
            size_candidates.insert(size_candidates.end(), members.begin(), members.end());
        }
    }
    report.candidate_file_count = size_candidates.size();
    if (size_candidates.empty()) {
        return report;
    }

    // 第 2 轮：快速哈希（只读前 16 KB）
    progress_total_files_ = size_candidates.size();
    const std::vector<std::optional<std::uint64_t>> quick_hashes = HashInParallel(size_candidates, kQuickHashBytes);
    CandidateGroups quick_groups;
    for (std::size_t index = 0; index < size_candidates.size(); ++index) {
        if (!quick_hashes[index]) {
            ++report.unreadable_file_count;
            continue;
        }
        quick_groups[SizeAndHash{size_candidates[index]->size, *quick_hashes[index]}].push_back(
            size_candidates[index]);  //call (anonymous namespace)::SizeAndHashHasher::operator() 与 SizeAndHash::operator==
    }

    // 不超过 16 KB 的文件，快速哈希已经覆盖全文，可以直接定案；更大的进入第 3 轮
    CandidateGroups final_groups;
    std::vector<const FileInfo*> full_candidates;
    for (auto& [key, members] : quick_groups) {
        if (members.size() < 2) {
            continue;
        }
        if (key.file_size <= kQuickHashBytes) {
            final_groups.emplace(key, std::move(members));
        } else {
            full_candidates.insert(full_candidates.end(), members.begin(), members.end());
        }
    }

    // 第 3 轮：全文哈希
    progress_total_files_ += full_candidates.size();
    const std::vector<std::optional<std::uint64_t>> full_hashes = HashInParallel(full_candidates, std::nullopt);
    for (std::size_t index = 0; index < full_candidates.size(); ++index) {
        if (!full_hashes[index]) {
            ++report.unreadable_file_count;
            continue;
        }
        final_groups[SizeAndHash{full_candidates[index]->size, *full_hashes[index]}].push_back(
            full_candidates[index]);  //call (anonymous namespace)::SizeAndHashHasher::operator() 与 SizeAndHash::operator==
    }

    for (const auto& [key, members] : final_groups) {
        if (members.size() < 2) {
            continue;
        }
        DuplicateGroup group;
        group.file_size = key.file_size;
        group.content_hash = key.content_hash;
        group.files.reserve(members.size());
        for (const FileInfo* member : members) {
            group.files.push_back(*member);
        }
        std::sort(group.files.begin(), group.files.end(), &FileOrdering::ByPathAscending);  //call filesearch::FileOrdering::ByPathAscending
        report.groups.push_back(std::move(group));
    }
    std::sort(report.groups.begin(), report.groups.end(), &CompareGroupsByReclaimableBytes);  //call (anonymous namespace)::CompareGroupsByReclaimableBytes

    report.hashed_bytes = hashed_bytes_.load();
    ReportProgress(true);
    return report;
}

}  // namespace filesearch::analysis
