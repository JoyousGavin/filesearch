#pragma once

#include "filesearch/CacheStore.hpp"
#include "filesearch/DuplicateDetector.hpp"
#include "filesearch/ExecutionTimer.hpp"
#include "filesearch/FileAnalyzer.hpp"
#include "filesearch/FileInfo.hpp"
#include "filesearch/ScanEngine.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

namespace filesearch::render {

enum class Alignment {
    Left,
    Right,
};

// RAII：保存并恢复流的格式状态。std::left / std::fixed / setprecision / setfill 都是“粘性”的，用完不恢复会影响后续输出
class StreamFormatGuard {
public:
    explicit StreamFormatGuard(std::ostream& stream)
        : stream_(stream), flags_(stream.flags()), precision_(stream.precision()), fill_(stream.fill()) {}
    ~StreamFormatGuard() {
        stream_.flags(flags_);
        stream_.precision(precision_);
        stream_.fill(fill_);
    }
    StreamFormatGuard(const StreamFormatGuard&) = delete;
    StreamFormatGuard& operator=(const StreamFormatGuard&) = delete;

private:
    std::ostream& stream_;
    std::ios_base::fmtflags flags_;
    std::streamsize precision_;
    char fill_;
};

class ConsoleRenderer {
public:
    // output 输出结果表格（stdout），diagnostics 输出进度和错误（stderr），这样结果可以单独重定向到文件
    ConsoleRenderer(std::ostream& output, std::ostream& diagnostics);

    // 进度回调目标：单行 '\r' 覆盖刷新
    void RenderScanProgress(const scan::ScanProgress& progress);
    void RenderHashProgress(const analysis::HashProgress& progress);

    void RenderScanSummary(const scan::ScanResult& scan_result, bool verbose);
    void RenderFilterSummary(std::size_t matched_count, std::uintmax_t matched_bytes, std::size_t scanned_count);
    void RenderFileTable(std::string_view title, std::span<const FileInfo> files, std::size_t total_count,
                         std::uintmax_t total_bytes);
    void RenderExtensionStatistics(std::span<const analysis::ExtensionStatistics> statistics,
                                   std::uintmax_t total_bytes, std::size_t row_limit);
    void RenderDuplicateReport(const analysis::DuplicateReport& report, std::size_t group_limit);

    void RenderCacheLoad(const cache::LoadResult& load_result, const std::filesystem::path& cache_file);
    void RenderCacheSaved(const std::filesystem::path& cache_file, const scan::ScanResult& scan_result);
    void RenderCacheUpToDate(const std::filesystem::path& cache_file);
    void RenderTimingReport(const timing::TimingReport& timing_report);
    void RenderBenchmark(std::span<const timing::BenchmarkResult> results, std::size_t file_count,
                         std::size_t repetition_count);

    static std::string FormatBytes(std::uintmax_t bytes);                   // 10240 -> "10.00 KB"
    static std::string FormatCount(std::uintmax_t count);                   // 1234567 -> "1,234,567"
    static std::string FormatDuration(std::chrono::nanoseconds duration);   // "1.23 s" / "45.6 ms" / "789 us"
    static std::string FormatTime(std::filesystem::file_time_type file_time);  // 本地时间 "2026-11-04 14:12"

    // 终端显示宽度：中日韩文字、全角符号、emoji 占 2 列，std::setw 按字节计数处理不了这种情况
    static std::size_t DisplayWidth(std::string_view utf8_text);
    static std::string PadToWidth(std::string_view utf8_text, std::size_t width, Alignment alignment);

private:
    void RenderTitle(std::string_view title);
    void RenderRule(std::size_t width);
    void WriteProgressLine(const std::string& line, bool finished);

    std::ostream& output_;
    std::ostream& diagnostics_;
    std::size_t spinner_index_ = 0;
    std::size_t progress_line_width_ = 0;
};

}  // namespace filesearch::render
