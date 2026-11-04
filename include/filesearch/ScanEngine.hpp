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

// 上一次扫描时某个目录的快照（由缓存模块提供）
struct DirectorySnapshot {
    std::optional<std::filesystem::file_time_type> last_write_time;  // std::nullopt：上次没能完整读取，不能复用
    std::vector<FileInfo> files;
    std::vector<std::filesystem::path> subdirectories;
};

// 扫描引擎只需要“按目录查询上次结果”这一个能力，用接口隔离，scan 模块因此不依赖 cache 模块
class DirectorySnapshotProvider {
public:
    virtual ~DirectorySnapshotProvider() = default;
    virtual const DirectorySnapshot* FindDirectory(const std::filesystem::path& directory) const = 0;

protected:
    // 拷贝/移动设为 protected：派生类可以正常拷贝移动，但不能通过基类引用发生切片
    DirectorySnapshotProvider() = default;
    DirectorySnapshotProvider(const DirectorySnapshotProvider&) = default;
    DirectorySnapshotProvider(DirectorySnapshotProvider&&) = default;
    DirectorySnapshotProvider& operator=(const DirectorySnapshotProvider&) = default;
    DirectorySnapshotProvider& operator=(DirectorySnapshotProvider&&) = default;
};

// 本次扫描访问过的目录，供缓存模块写入
struct DirectoryRecord {
    std::filesystem::path path;
    std::optional<std::filesystem::file_time_type> last_write_time;  // std::nullopt：本次没能完整读取
};

struct ScanOptions {
    std::filesystem::path root_directory;
    std::size_t thread_count = 0;  // 0 表示使用 std::thread::hardware_concurrency()
    ScanStrategy strategy = ScanStrategy::WorkQueue;
    std::chrono::milliseconds progress_interval{100};
    // 以下两项只对 WorkQueue 策略生效：
    // 目录的最后修改时间与快照一致时（说明没有增删改名条目），直接复用快照里的文件列表，不再遍历该目录
    const DirectorySnapshotProvider* previous_snapshot = nullptr;
    bool record_directories = false;  // 是否在结果中记录 DirectoryRecord（写缓存时需要）
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
    std::uintmax_t total_bytes = 0;
    std::size_t thread_count = 0;
    ScanStrategy strategy = ScanStrategy::WorkQueue;
    std::chrono::steady_clock::duration elapsed{};
    std::vector<DirectoryRecord> directories;  // 仅在 record_directories 为 true 时填充
    std::size_t reused_directory_count = 0;    // 直接复用快照、没有重新遍历的目录数
};

std::string_view ToString(ScanStrategy strategy);

// 转成绝对路径并规范化（去掉 "."、".." 和末尾多余的分隔符），保证缓存键与本次扫描产生的路径一致
std::filesystem::path NormalizeRootDirectory(const std::filesystem::path& root_directory);

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

    // 工作队列中取出的目录：能复用快照就复用，否则遍历
    void ProcessQueuedDirectory(const std::filesystem::path& directory, std::vector<FileInfo>& local_files,
                                std::vector<std::filesystem::path>& discovered_subdirectories,
                                std::vector<DirectoryRecord>& local_directory_records);
    // 遍历一层目录；目录打开失败或遍历中断时返回 false
    bool EnumerateDirectory(const std::filesystem::path& directory, std::vector<FileInfo>& local_files,
                            std::vector<std::filesystem::path>& discovered_subdirectories);
    void ScanSubtreeRecursively(const std::filesystem::path& subtree_root, std::vector<FileInfo>& task_files);

    void AppendFiles(std::vector<FileInfo>& local_files);
    void AppendDirectoryRecords(std::vector<DirectoryRecord>& local_directory_records);
    void RecordError(const std::filesystem::path& path, const std::error_code& error, std::string_view context);
    void RecordException(std::exception_ptr exception);
    ScanProgress SnapshotProgress() const;

    ScanOptions options_;
    ProgressCallback progress_callback_;
    std::chrono::steady_clock::time_point start_time_;

    // 结果容器：多个线程批量写入，由 results_mutex_ 保护
    std::mutex results_mutex_;
    std::vector<FileInfo> files_;
    std::vector<DirectoryRecord> directory_records_;

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
    std::atomic<std::size_t> reused_directories_{0};
};

std::size_t ResolveThreadCount(std::size_t requested_thread_count);

}  // namespace filesearch::scan
