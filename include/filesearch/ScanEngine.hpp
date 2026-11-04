#pragma once

#include "filesearch/FileInfo.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace filesearch::scan {

enum class ScanStrategy {
    WorkQueue,             // 固定数量 std::thread + 共享目录队列（动态负载均衡，默认）
    AsyncPerSubdirectory,  // 根目录的每个子文件夹一个任务，std::async 并发，各自用 recursive_directory_iterator
};

struct ScanOptions {
    std::filesystem::path root_directory;
    std::size_t thread_count = 0;  // 0 表示使用 std::thread::hardware_concurrency()
    ScanStrategy strategy = ScanStrategy::WorkQueue;
    std::chrono::milliseconds progress_interval{100};
};

// 进度快照：由进度线程从原子计数器读取后交给回调
struct ScanProgress {
    std::size_t scanned_files = 0;
    std::size_t scanned_directories = 0;
    std::uintmax_t scanned_bytes = 0;
    std::size_t error_count = 0;
    std::chrono::steady_clock::duration elapsed{};
    bool finished = false;
};

struct ScanResult {
    std::vector<FileInfo> files;
    std::vector<std::string> error_messages;  // 只保留前若干条，完整数量见 error_count
    std::size_t error_count = 0;
    std::size_t directory_count = 0;
    std::size_t thread_count = 0;
    std::chrono::steady_clock::duration elapsed{};
};

using ProgressCallback = std::function<void(const ScanProgress&)>;

class ScanEngine {
public:
    explicit ScanEngine(ScanOptions options);

    void SetProgressCallback(ProgressCallback progress_callback);

    // 阻塞直到扫描结束。工作线程里的异常会被收集并在这里重新抛出。
    ScanResult Run();

    // 由 directory_entry 构造 FileInfo；任何属性读取失败都返回 std::nullopt
    static std::optional<FileInfo> MakeFileInfo(const std::filesystem::directory_entry& entry,
                                                std::error_code& entry_error);

private:
    void RunWorkQueue();
    void RunAsyncPerSubdirectory();

    void WorkerLoop();
    std::vector<FileInfo> AsyncTaskLoop(const std::vector<std::filesystem::path>& subdirectories,
                                        std::atomic<std::size_t>& next_subdirectory_index);
    void ProgressLoop(std::stop_token stop_token);

    void ScanOneDirectory(const std::filesystem::path& directory, std::vector<FileInfo>& local_files,
                          std::vector<std::filesystem::path>& discovered_subdirectories);
    void ScanSubtreeRecursively(const std::filesystem::path& subtree_root, std::vector<FileInfo>& task_files);

    void AppendFiles(std::vector<FileInfo>& local_files);
    void RecordError(const std::filesystem::path& path, const std::error_code& error, std::string_view context);
    void RecordException(std::exception_ptr exception);
    ScanProgress SnapshotProgress() const;

    ScanOptions options_;
    ProgressCallback progress_callback_;
    std::chrono::steady_clock::time_point start_time_;

    // 结果容器：多个线程批量写入，由 results_mutex_ 保护
    std::mutex results_mutex_;
    std::vector<FileInfo> files_;

    // 目录工作队列：queue_mutex_ 保护下面三个成员，queue_condition_ 用于唤醒空闲线程
    std::mutex queue_mutex_;
    std::condition_variable queue_condition_;
    std::deque<std::filesystem::path> pending_directories_;
    std::size_t active_workers_ = 0;
    bool abort_requested_ = false;

    // 错误信息与线程异常
    std::mutex errors_mutex_;
    std::vector<std::string> error_messages_;
    std::exception_ptr first_exception_;

    // 进度计数器：工作线程 relaxed 递增，进度线程定期读取
    std::atomic<std::size_t> scanned_files_{0};
    std::atomic<std::size_t> scanned_directories_{0};
    std::atomic<std::uintmax_t> scanned_bytes_{0};
    std::atomic<std::size_t> error_count_{0};
};

std::size_t ResolveThreadCount(std::size_t requested_thread_count);

}  // namespace filesearch::scan
