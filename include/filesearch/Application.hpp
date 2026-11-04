#pragma once

#include "filesearch/CommandLineParser.hpp"
#include "filesearch/ConsoleRenderer.hpp"
#include "filesearch/ExecutionTimer.hpp"
#include "filesearch/FileFilter.hpp"
#include "filesearch/FileInfo.hpp"
#include "filesearch/ScanEngine.hpp"

#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

namespace filesearch::app {

inline constexpr int kExitSuccess = 0;
inline constexpr int kExitRuntimeError = 1;
inline constexpr int kExitUsageError = 2;

// 把各模块串成流水线：[缓存读取] -> 扫描 -> [缓存写入] -> 过滤 -> 按运行模式分析 -> 渲染
class Application {
public:
    Application(std::ostream& output, std::ostream& diagnostics, timing::TimingReport& timing_report);

    int Run(const cli::CommandLineOptions& options);

private:
    scan::ScanResult ScanWithCache(scan::ScanOptions scan_options, const cli::CommandLineOptions& options);

    void RunSearchMode(std::vector<FileInfo>& matched_files, std::uintmax_t matched_bytes,
                       const cli::CommandLineOptions& options);
    void RunStatisticsMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                           const cli::CommandLineOptions& options);
    void RunTopMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                    const cli::CommandLineOptions& options);
    void RunDuplicateMode(std::span<const FileInfo> matched_files, const cli::CommandLineOptions& options);
    void RunBenchmarkMode(std::span<const FileInfo> all_files, std::span<const FileInfo> matched_files,
                          const filter::FileFilter& file_filter, const cli::CommandLineOptions& options);

    std::ostream& diagnostics_;
    render::ConsoleRenderer renderer_;
    timing::TimingReport& timing_report_;
};

}  // namespace filesearch::app
