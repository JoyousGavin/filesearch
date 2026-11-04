// 阶段 3：条件过滤与正则表达式
//   命令行参数（状态机解析）-> 多线程扫描 -> FileFilter 过滤（copy_if + regex）-> 排序 -> 打印
#include "filesearch/CommandLineParser.hpp"
#include "filesearch/FileFilter.hpp"
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
#include <regex>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitRuntimeError = 1;
constexpr int kExitUsageError = 2;

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

int RunSearch(const filesearch::cli::CommandLineOptions& options) {
    std::error_code status_error;
    if (!std::filesystem::is_directory(options.scan.root_directory, status_error)) {
        std::cerr << "不是有效目录: " << filesearch::text::PathToUtf8(options.scan.root_directory) << '\n';
        return kExitUsageError;
    }

    // 先编译正则再扫描：模式写错时立即报错，不必白白等扫描结束
    std::optional<filesearch::filter::FileFilter> file_filter;
    try {
        file_filter.emplace(options.criteria);
    } catch (const std::regex_error& regex_error) {
        std::cerr << "正则表达式无效: " << regex_error.what() << '\n';
        return kExitUsageError;
    }

    filesearch::scan::ScanEngine scan_engine(options.scan);
    scan_engine.SetProgressCallback(&PrintScanProgress);
    const filesearch::scan::ScanResult scan_result = scan_engine.Run();

    std::vector<filesearch::FileInfo> matched_files = file_filter->Apply(scan_result.files);
    filesearch::filter::FileFilter::Sort(matched_files, options.sort_key);

    std::uintmax_t matched_bytes = 0;
    for (const filesearch::FileInfo& file : matched_files) {
        matched_bytes += file.size;
    }

    const std::size_t print_count =
        options.display_limit == 0 ? matched_files.size() : std::min(matched_files.size(), options.display_limit);
    for (std::size_t index = 0; index < print_count; ++index) {
        std::cout << matched_files[index].size << " bytes  " << filesearch::text::PathToUtf8(matched_files[index].path)
                  << '\n';
    }
    if (print_count < matched_files.size()) {
        std::cout << "... 还有 " << matched_files.size() - print_count << " 条未显示（--limit 0 显示全部）\n";
    }
    std::cout << "匹配 " << matched_files.size() << " / " << scan_result.files.size() << " 个文件, 共 " << matched_bytes
              << " bytes; 线程 " << scan_result.thread_count << ", 错误 " << scan_result.error_count << " 处\n";
    return kExitSuccess;
}

}  // namespace

int main(int argument_count, char** argument_values) {
    const filesearch::platform::ConsoleEncodingGuard console_encoding_guard;
    const std::vector<std::string> arguments =
        filesearch::platform::CollectUtf8Arguments(argument_count, argument_values);

    filesearch::cli::CommandLineParser command_line_parser;
    const filesearch::cli::ParseOutcome parse_outcome = command_line_parser.Parse(arguments);

    if (std::holds_alternative<filesearch::cli::HelpRequest>(parse_outcome)) {
        filesearch::cli::CommandLineParser::PrintUsage(std::cout);
        return kExitSuccess;
    }
    if (const auto* parse_error = std::get_if<filesearch::cli::ParseError>(&parse_outcome)) {
        std::cerr << "参数错误: " << parse_error->message << "\n使用 --help 查看用法\n";
        return kExitUsageError;
    }

    try {
        return RunSearch(std::get<filesearch::cli::CommandLineOptions>(parse_outcome));
    } catch (const std::exception& exception) {
        std::cerr << "\n运行失败: " << exception.what() << '\n';
        return kExitRuntimeError;
    }
}
