#pragma once

#include "filesearch/CommandLineParser.hpp"
#include "filesearch/ConsoleRenderer.hpp"
#include "filesearch/FileInfo.hpp"

#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

namespace filesearch::app {

inline constexpr int kExitSuccess = 0;
inline constexpr int kExitRuntimeError = 1;
inline constexpr int kExitUsageError = 2;

// 把各模块串成流水线：扫描 -> 过滤 -> 按运行模式分析 -> 渲染
class Application {
public:
    Application(std::ostream& output, std::ostream& diagnostics);

    int Run(const cli::CommandLineOptions& options);

private:
    void RunSearchMode(std::vector<FileInfo>& matched_files, std::uintmax_t matched_bytes,
                       const cli::CommandLineOptions& options);
    void RunStatisticsMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                           const cli::CommandLineOptions& options);
    void RunTopMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                    const cli::CommandLineOptions& options);
    void RunDuplicateMode(std::span<const FileInfo> matched_files, const cli::CommandLineOptions& options);

    std::ostream& diagnostics_;
    render::ConsoleRenderer renderer_;
};

}  // namespace filesearch::app
