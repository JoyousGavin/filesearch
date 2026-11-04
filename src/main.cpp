// 阶段 1：单线程雏形
//   recursive_directory_iterator 扫目录 -> std::vector 存结果 -> std::sort 按大小排序 -> std::cout 打印
#include "filesearch/FileInfo.hpp"
#include "filesearch/Platform.hpp"
#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kPrintLimit = 20;

bool CompareBySizeDescending(const filesearch::FileInfo& left, const filesearch::FileInfo& right) {
    return left.size > right.size;
}

std::vector<filesearch::FileInfo> ScanSingleThreaded(const std::filesystem::path& root_directory,
                                                     std::size_t& error_count) {
    std::vector<filesearch::FileInfo> files;
    std::error_code iteration_error;
    std::filesystem::recursive_directory_iterator iterator(
        root_directory, std::filesystem::directory_options::skip_permission_denied, iteration_error);
    const std::filesystem::recursive_directory_iterator end_iterator;

    // 注意：带 error_code 的 increment 一旦失败，迭代器会变成 end，后面的目录全部丢失。
    // 这正是阶段 2 改为“每个目录单独用 directory_iterator 遍历”的原因之一。
    for (; !iteration_error && iterator != end_iterator; iterator.increment(iteration_error)) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entry_error;

        // 不进入符号链接/联接点指向的目录，避免重复统计和循环
        if (entry.is_symlink(entry_error)) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(entry_error)) {
            continue;
        }

        filesearch::FileInfo file;
        file.path = entry.path();
        file.extension = filesearch::text::ToLowerAscii(filesearch::text::PathToUtf8(file.path.extension()));
        file.size = entry.file_size(entry_error);
        if (entry_error) {
            ++error_count;
            continue;
        }
        file.last_write_time = entry.last_write_time(entry_error);
        files.push_back(std::move(file));
    }

    if (iteration_error) {
        ++error_count;
        std::cerr << "遍历中断: " << iteration_error.message() << '\n';
    }
    return files;
}

}  // namespace

int main(int argument_count, char** argument_values) {
    const filesearch::platform::ConsoleEncodingGuard console_encoding_guard;
    const std::vector<std::string> arguments =
        filesearch::platform::CollectUtf8Arguments(argument_count, argument_values);

    const std::filesystem::path root_directory =
        arguments.size() > 1 ? filesearch::text::PathFromUtf8(arguments[1]) : std::filesystem::current_path();

    std::error_code status_error;
    if (!std::filesystem::is_directory(root_directory, status_error)) {
        std::cerr << "不是有效目录: " << filesearch::text::PathToUtf8(root_directory) << '\n';
        return 1;
    }

    std::size_t error_count = 0;
    std::vector<filesearch::FileInfo> files = ScanSingleThreaded(root_directory, error_count);

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
    std::cout << "共 " << files.size() << " 个文件, " << total_bytes << " bytes, 错误 " << error_count << " 处\n";
    return 0;
}
