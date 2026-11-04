// 阶段 2：引入并发与原子操作
//   用法（临时，阶段 3 换成正式的命令行解析）：filesearch [目录] [线程数] [queue|async]
#include "filesearch/FileInfo.hpp"
#include "filesearch/Platform.hpp"
#include "filesearch/ScanEngine.hpp"
#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::size_t kPrintLimit = 20;

bool CompareBySizeDescending(const filesearch::FileInfo& left, const filesearch::FileInfo& right) {
    return left.size > right.size;
}

// 进度回调：由扫描引擎的进度线程每 100ms 调用一次，结束时由 Run() 再调用一次（finished = true）
void PrintScanProgress(const filesearch::scan::ScanProgress& progress) {
    const auto elapsed_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(progress.elapsed);
    std::cerr << "\r已扫描 " << progress.scanned_files << " 个文件, " << progress.scanned_directories << " 个目录, "
              << progress.scanned_bytes << " bytes, 错误 " << progress.error_count << ", "
              << elapsed_milliseconds.count() << " ms   " << std::flush;
    if (progress.finished) {
        std::cerr << '\n';
    }
}

}  // namespace

int main(int argument_count, char** argument_values) {
    const filesearch::platform::ConsoleEncodingGuard console_encoding_guard;
    const std::vector<std::string> arguments =
        filesearch::platform::CollectUtf8Arguments(argument_count, argument_values);

    filesearch::scan::ScanOptions scan_options;
    scan_options.root_directory =
        arguments.size() > 1 ? filesearch::text::PathFromUtf8(arguments[1]) : std::filesystem::current_path();
    if (arguments.size() > 2) {
        scan_options.thread_count = static_cast<std::size_t>(std::stoul(arguments[2]));
    }
    if (arguments.size() > 3 && arguments[3] == "async") {
        scan_options.strategy = filesearch::scan::ScanStrategy::AsyncPerSubdirectory;
    }

    std::error_code status_error;
    if (!std::filesystem::is_directory(scan_options.root_directory, status_error)) {
        std::cerr << "不是有效目录: " << filesearch::text::PathToUtf8(scan_options.root_directory) << '\n';
        return 1;
    }

    try {
        filesearch::scan::ScanEngine scan_engine(scan_options);
        scan_engine.SetProgressCallback(&PrintScanProgress);
        filesearch::scan::ScanResult scan_result = scan_engine.Run();
        std::vector<filesearch::FileInfo>& files = scan_result.files;

        std::sort(files.begin(), files.end(), CompareBySizeDescending);  //call (anonymous namespace)::CompareBySizeDescending

        std::uintmax_t total_bytes = 0;
        for (const filesearch::FileInfo& file : files) {
            total_bytes += file.size;
        }

        const std::size_t print_count = std::min(files.size(), kPrintLimit);
        std::cout << "最大的 " << print_count << " 个文件:\n";
        for (std::size_t index = 0; index < print_count; ++index) {
            std::cout << files[index].size << " bytes  " << filesearch::text::PathToUtf8(files[index].path) << '\n';
        }
        std::cout << "共 " << files.size() << " 个文件, " << scan_result.directory_count << " 个目录, " << total_bytes
                  << " bytes, 线程 " << scan_result.thread_count << ", 错误 " << scan_result.error_count << " 处\n";
        for (const std::string& error_message : scan_result.error_messages) {
            std::cerr << "  " << error_message << '\n';
        }
    } catch (const std::exception& exception) {
        std::cerr << "扫描失败: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
