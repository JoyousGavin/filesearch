#include "filesearch/ScanEngine.hpp"

#include "filesearch/Platform.hpp"
#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <future>
#include <iterator>
#include <thread>
#include <utility>

namespace filesearch::scan {

namespace {

constexpr std::size_t kLocalFlushThreshold = 4096;  // 线程本地缓冲攒够这么多条再加锁合并，减少锁竞争
constexpr std::size_t kMaximumStoredErrors = 50;
constexpr std::size_t kMaximumThreadCount = 256;
constexpr std::size_t kFallbackThreadCount = 4;

}  // namespace

std::string_view ToString(ScanStrategy strategy) {
    switch (strategy) {
        case ScanStrategy::WorkQueue:
            return "queue";
        case ScanStrategy::AsyncPerSubdirectory:
            return "async";
    }
    return "unknown";
}

std::size_t ResolveThreadCount(std::size_t requested_thread_count) {
    if (requested_thread_count == 0) {
        const unsigned int hardware_threads = std::thread::hardware_concurrency();  // 无法检测时返回 0
        requested_thread_count = hardware_threads == 0 ? kFallbackThreadCount : hardware_threads;
    }
    return std::clamp<std::size_t>(requested_thread_count, 1, kMaximumThreadCount);
}

ScanEngine::ScanEngine(ScanOptions options) : options_(std::move(options)) {
    options_.thread_count = ResolveThreadCount(options_.thread_count);
}

void ScanEngine::SetProgressCallback(ProgressCallback progress_callback) {
    progress_callback_ = std::move(progress_callback);
}

ScanResult ScanEngine::Run() {
    start_time_ = std::chrono::steady_clock::now();
    {
        // jthread 析构时自动 request_stop() + join()，即使扫描过程抛异常也不会留下野线程
        std::jthread progress_thread;
        if (progress_callback_) {
            progress_thread = std::jthread([this](std::stop_token stop_token) {
                ProgressLoop(stop_token);  //call filesearch::scan::ScanEngine::ProgressLoop
            });
        }
        switch (options_.strategy) {
            case ScanStrategy::WorkQueue:
                RunWorkQueue();
                break;
            case ScanStrategy::AsyncPerSubdirectory:
                RunAsyncPerSubdirectory();
                break;
        }
    }

    if (first_exception_) {
        std::rethrow_exception(first_exception_);
    }

    ScanProgress final_progress = SnapshotProgress();
    final_progress.finished = true;
    if (progress_callback_) {
        progress_callback_(final_progress);  //call filesearch::render::ConsoleRenderer::RenderScanProgress（由 Application 通过 SetProgressCallback 注册）
    }

    ScanResult result;
    result.files = std::move(files_);
    result.error_messages = std::move(error_messages_);
    result.error_count = error_count_.load();
    result.directory_count = scanned_directories_.load();
    result.total_bytes = scanned_bytes_.load();
    result.thread_count = options_.thread_count;
    result.strategy = options_.strategy;
    result.elapsed = final_progress.elapsed;
    return result;
}

std::optional<FileInfo> ScanEngine::MakeFileInfo(const std::filesystem::directory_entry& entry,
                                                 std::error_code& entry_error) {
    FileInfo file;
    file.size = entry.file_size(entry_error);
    if (entry_error) {
        return std::nullopt;
    }
    file.last_write_time = entry.last_write_time(entry_error);
    if (entry_error) {
        return std::nullopt;
    }
    file.path = entry.path();
    file.extension = text::ToLowerAscii(text::PathToUtf8(file.path.extension()));
    return file;
}

// ---------------------------------------------------------------------------
// 策略一：固定数量的 std::thread + 共享目录队列
// ---------------------------------------------------------------------------

void ScanEngine::RunWorkQueue() {
    {
        const std::lock_guard<std::mutex> lock(queue_mutex_);
        pending_directories_.push_back(options_.root_directory);
    }

    std::vector<std::thread> workers;
    workers.reserve(options_.thread_count);
    try {
        for (std::size_t worker_index = 0; worker_index < options_.thread_count; ++worker_index) {
            workers.emplace_back(&ScanEngine::WorkerLoop, this);  //call filesearch::scan::ScanEngine::WorkerLoop
        }
    } catch (...) {
        // 创建线程失败（std::system_error）：让已启动的线程退出并 join，否则 std::thread 析构会 std::terminate
        {
            const std::lock_guard<std::mutex> lock(queue_mutex_);
            abort_requested_ = true;
        }
        queue_condition_.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
        throw;
    }

    for (std::thread& worker : workers) {
        worker.join();
    }
}

void ScanEngine::WorkerLoop() {
    std::vector<FileInfo> local_files;
    std::vector<std::filesystem::path> discovered_subdirectories;
    try {
        while (true) {
            std::filesystem::path directory;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                // 醒来条件：有目录可取 / 再也不会有新目录（队列空且没有线程在扫描）/ 被要求中止
                queue_condition_.wait(lock, [this] {
                    return abort_requested_ || !pending_directories_.empty() || active_workers_ == 0;
                });  //call 上面的 lambda 谓词（std::condition_variable::wait 内部反复调用）
                if (abort_requested_ || pending_directories_.empty()) {
                    break;
                }
                directory = std::move(pending_directories_.back());  // LIFO：近似深度优先，队列峰值更小
                pending_directories_.pop_back();
                ++active_workers_;
            }

            ScanOneDirectory(directory, local_files, discovered_subdirectories);

            bool scan_finished = false;
            {
                const std::lock_guard<std::mutex> lock(queue_mutex_);
                --active_workers_;
                std::move(discovered_subdirectories.begin(), discovered_subdirectories.end(),
                          std::back_inserter(pending_directories_));
                scan_finished = active_workers_ == 0 && pending_directories_.empty();
            }
            if (scan_finished || discovered_subdirectories.size() > 1) {
                queue_condition_.notify_all();
            } else if (discovered_subdirectories.size() == 1) {
                queue_condition_.notify_one();
            }
            discovered_subdirectories.clear();

            if (local_files.size() >= kLocalFlushThreshold) {
                AppendFiles(local_files);
            }
        }
        AppendFiles(local_files);
    } catch (...) {
        // 异常不能逃出线程函数（否则 std::terminate），先保存，等 join 之后在 Run() 中重新抛出
        RecordException(std::current_exception());
        {
            const std::lock_guard<std::mutex> lock(queue_mutex_);
            abort_requested_ = true;
        }
        queue_condition_.notify_all();
    }
}

void ScanEngine::ScanOneDirectory(const std::filesystem::path& directory, std::vector<FileInfo>& local_files,
                                  std::vector<std::filesystem::path>& discovered_subdirectories) {
    std::error_code iteration_error;
    std::filesystem::directory_iterator iterator(
        directory, std::filesystem::directory_options::skip_permission_denied, iteration_error);
    if (iteration_error) {
        RecordError(directory, iteration_error, "无法打开目录");
        return;
    }
    scanned_directories_.fetch_add(1, std::memory_order_relaxed);

    const std::filesystem::directory_iterator end_iterator{};
    for (; iterator != end_iterator; iterator.increment(iteration_error)) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entry_error;
        // symlink_status 不跟随链接：符号链接不会被当作目录递归进去，避免重复统计和循环
        const std::filesystem::file_status link_status = entry.symlink_status(entry_error);
        if (entry_error) {
            RecordError(entry.path(), entry_error, "无法读取属性");
            continue;
        }
        if (std::filesystem::is_directory(link_status)) {
            discovered_subdirectories.push_back(entry.path());
            continue;
        }
        if (!std::filesystem::is_regular_file(link_status)) {
            continue;
        }
        if (std::optional<FileInfo> file = MakeFileInfo(entry, entry_error)) {
            scanned_files_.fetch_add(1, std::memory_order_relaxed);
            scanned_bytes_.fetch_add(file->size, std::memory_order_relaxed);
            local_files.push_back(std::move(*file));
        } else {
            RecordError(entry.path(), entry_error, "无法读取文件信息");
        }
    }
    // increment 失败时迭代器变为 end，循环结束；这里只丢失当前这一个目录的剩余条目
    if (iteration_error) {
        RecordError(directory, iteration_error, "目录遍历中断");
    }
}

// ---------------------------------------------------------------------------
// 策略二：根目录的每个子文件夹一个任务，std::async 并发
// 缺点：负载按顶层子文件夹静态划分，某个子文件夹特别大时其余线程会提前空闲
// ---------------------------------------------------------------------------

void ScanEngine::RunAsyncPerSubdirectory() {
    std::vector<FileInfo> root_files;
    std::vector<std::filesystem::path> top_level_subdirectories;
    ScanOneDirectory(options_.root_directory, root_files, top_level_subdirectories);
    AppendFiles(root_files);

    // 任务数不超过线程数；每个任务通过原子下标领取下一个子文件夹
    std::atomic<std::size_t> next_subdirectory_index{0};
    const std::size_t task_count = std::min(options_.thread_count, top_level_subdirectories.size());
    std::vector<std::future<std::vector<FileInfo>>> futures;
    futures.reserve(task_count);
    for (std::size_t task_index = 0; task_index < task_count; ++task_index) {
        futures.push_back(std::async(std::launch::async, &ScanEngine::AsyncTaskLoop, this,
                                     std::cref(top_level_subdirectories),
                                     std::ref(next_subdirectory_index)));  //call filesearch::scan::ScanEngine::AsyncTaskLoop
    }

    for (std::future<std::vector<FileInfo>>& future : futures) {
        std::vector<FileInfo> task_files = future.get();  // 任务内抛出的异常会在 get() 处重新抛出
        AppendFiles(task_files);
    }
}

std::vector<FileInfo> ScanEngine::AsyncTaskLoop(const std::vector<std::filesystem::path>& subdirectories,
                                                std::atomic<std::size_t>& next_subdirectory_index) {
    std::vector<FileInfo> task_files;
    for (std::size_t index = next_subdirectory_index.fetch_add(1); index < subdirectories.size();
         index = next_subdirectory_index.fetch_add(1)) {
        ScanSubtreeRecursively(subdirectories[index], task_files);
    }
    return task_files;
}

void ScanEngine::ScanSubtreeRecursively(const std::filesystem::path& subtree_root, std::vector<FileInfo>& task_files) {
    std::error_code iteration_error;
    std::filesystem::recursive_directory_iterator iterator(
        subtree_root, std::filesystem::directory_options::skip_permission_denied, iteration_error);
    if (iteration_error) {
        RecordError(subtree_root, iteration_error, "无法打开目录");
        return;
    }
    scanned_directories_.fetch_add(1, std::memory_order_relaxed);

    const std::filesystem::recursive_directory_iterator end_iterator{};
    for (; iterator != end_iterator; iterator.increment(iteration_error)) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entry_error;
        const std::filesystem::file_status link_status = entry.symlink_status(entry_error);
        if (entry_error) {
            RecordError(entry.path(), entry_error, "无法读取属性");
            continue;
        }
        if (std::filesystem::is_directory(link_status)) {
            scanned_directories_.fetch_add(1, std::memory_order_relaxed);
            continue;  // 递归由迭代器自己完成
        }
        if (!std::filesystem::is_regular_file(link_status)) {
            iterator.disable_recursion_pending();  // 符号链接等：不要进入
            continue;
        }
        if (std::optional<FileInfo> file = MakeFileInfo(entry, entry_error)) {
            scanned_files_.fetch_add(1, std::memory_order_relaxed);
            scanned_bytes_.fetch_add(file->size, std::memory_order_relaxed);
            task_files.push_back(std::move(*file));
        } else {
            RecordError(entry.path(), entry_error, "无法读取文件信息");
        }
    }
    // recursive_directory_iterator 的 increment 一旦失败就变成 end，该子树剩余部分全部丢失
    if (iteration_error) {
        RecordError(subtree_root, iteration_error, "子树遍历中断，剩余部分未扫描");
    }
}

// ---------------------------------------------------------------------------
// 公共辅助
// ---------------------------------------------------------------------------

void ScanEngine::AppendFiles(std::vector<FileInfo>& local_files) {
    if (local_files.empty()) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(results_mutex_);
        files_.insert(files_.end(), std::make_move_iterator(local_files.begin()),
                      std::make_move_iterator(local_files.end()));
    }
    local_files.clear();
}

void ScanEngine::RecordError(const std::filesystem::path& path, const std::error_code& error,
                             std::string_view context) {
    error_count_.fetch_add(1, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock(errors_mutex_);
    if (error_messages_.size() >= kMaximumStoredErrors) {
        return;
    }
    std::string message(context);
    message.append(": ").append(text::PathToUtf8(path)).append(" -> ").append(platform::DescribeError(error));
    error_messages_.push_back(std::move(message));
}

void ScanEngine::RecordException(std::exception_ptr exception) {
    const std::lock_guard<std::mutex> lock(errors_mutex_);
    if (!first_exception_) {
        first_exception_ = std::move(exception);
    }
}

ScanProgress ScanEngine::SnapshotProgress() const {
    ScanProgress progress;
    progress.scanned_files = scanned_files_.load(std::memory_order_relaxed);
    progress.scanned_directories = scanned_directories_.load(std::memory_order_relaxed);
    progress.scanned_bytes = scanned_bytes_.load(std::memory_order_relaxed);
    progress.error_count = error_count_.load(std::memory_order_relaxed);
    progress.elapsed = std::chrono::steady_clock::now() - start_time_;
    return progress;
}

void ScanEngine::ProgressLoop(std::stop_token stop_token) {
    std::mutex wait_mutex;
    std::condition_variable_any wait_condition;
    std::unique_lock<std::mutex> wait_lock(wait_mutex);
    try {
        // C++20：wait_for 带 stop_token 的重载在超时或 request_stop() 时返回，停止请求能立即打断等待
        while (!wait_condition.wait_for(wait_lock, stop_token, options_.progress_interval, [] { return false; })) {  //call 恒为 false 的谓词，只靠超时/停止请求返回
            if (stop_token.stop_requested()) {
                break;
            }
            progress_callback_(SnapshotProgress());  //call filesearch::render::ConsoleRenderer::RenderScanProgress（由 Application 通过 SetProgressCallback 注册）
        }
    } catch (...) {
        RecordException(std::current_exception());
    }
}

}  // namespace filesearch::scan
