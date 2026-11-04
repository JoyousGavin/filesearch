#include "filesearch/Application.hpp"

#include "filesearch/DuplicateDetector.hpp"
#include "filesearch/FileAnalyzer.hpp"
#include "filesearch/FileFilter.hpp"
#include "filesearch/ScanEngine.hpp"
#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <regex>
#include <string_view>
#include <system_error>
#include <utility>

namespace filesearch::app {

namespace {

std::string_view DescribeSortKey(filter::SortKey sort_key) {
    switch (sort_key) {
        case filter::SortKey::Size:
            return "按大小降序";
        case filter::SortKey::Path:
            return "按路径升序";
        case filter::SortKey::Time:
            return "按修改时间降序";
    }
    return "";
}

std::size_t ApplyLimit(std::size_t available_count, std::size_t display_limit) {
    return display_limit == 0 ? available_count : std::min(available_count, display_limit);
}

}  // namespace

Application::Application(std::ostream& output, std::ostream& diagnostics)
    : diagnostics_(diagnostics), renderer_(output, diagnostics) {}

int Application::Run(const cli::CommandLineOptions& options) {
    std::error_code status_error;
    if (!std::filesystem::is_directory(options.scan.root_directory, status_error)) {
        diagnostics_ << "不是有效目录: " << text::PathToUtf8(options.scan.root_directory) << '\n';
        return kExitUsageError;
    }

    // 先编译正则再扫描：模式写错时立即报错，不必白白等扫描结束
    std::optional<filter::FileFilter> file_filter;
    try {
        file_filter.emplace(options.criteria);
    } catch (const std::regex_error& regex_error) {
        diagnostics_ << "正则表达式无效: " << regex_error.what() << '\n';
        return kExitUsageError;
    }

    scan::ScanEngine scan_engine(options.scan);
    scan_engine.SetProgressCallback([this](const scan::ScanProgress& progress) {
        renderer_.RenderScanProgress(progress);  //call filesearch::render::ConsoleRenderer::RenderScanProgress
    });
    scan::ScanResult scan_result = scan_engine.Run();
    renderer_.RenderScanSummary(scan_result, options.verbose);

    // 没有筛选条件时直接接管扫描结果，省掉一次全量拷贝
    const std::size_t scanned_count = scan_result.files.size();
    std::vector<FileInfo> matched_files =
        file_filter->HasConditions() ? file_filter->Apply(scan_result.files) : std::move(scan_result.files);
    const std::uintmax_t matched_bytes = analysis::FileAnalyzer::SumBytes(matched_files);
    if (file_filter->HasConditions()) {
        renderer_.RenderFilterSummary(matched_files.size(), matched_bytes, scanned_count);
    }

    for (const cli::RunMode mode : options.modes) {
        switch (mode) {
            case cli::RunMode::Search:
                RunSearchMode(matched_files, matched_bytes, options);
                break;
            case cli::RunMode::Statistics:
                RunStatisticsMode(matched_files, matched_bytes, options);
                break;
            case cli::RunMode::TopFiles:
                RunTopMode(matched_files, matched_bytes, options);
                break;
            case cli::RunMode::Duplicates:
                RunDuplicateMode(matched_files, options);
                break;
        }
    }
    return kExitSuccess;
}

void Application::RunSearchMode(std::vector<FileInfo>& matched_files, std::uintmax_t matched_bytes,
                                const cli::CommandLineOptions& options) {
    filter::FileFilter::Sort(matched_files, options.sort_key);
    const std::span<const FileInfo> shown_files =
        std::span<const FileInfo>(matched_files).first(ApplyLimit(matched_files.size(), options.display_limit));
    renderer_.RenderFileTable(std::format("搜索结果（{}）", DescribeSortKey(options.sort_key)), shown_files,
                              matched_files.size(), matched_bytes);
}

void Application::RunStatisticsMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                                    const cli::CommandLineOptions& options) {
    const std::vector<analysis::ExtensionStatistics> statistics =
        analysis::FileAnalyzer::ComputeExtensionStatistics(matched_files);
    renderer_.RenderExtensionStatistics(statistics, matched_bytes, options.display_limit);
}

void Application::RunTopMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                             const cli::CommandLineOptions& options) {
    const std::vector<FileInfo> top_files =
        analysis::FileAnalyzer::TopLargest(matched_files, options.top_count, options.top_algorithm);
    renderer_.RenderFileTable(std::format("最大的 {} 个文件（{}）", options.top_count,
                                          analysis::ToString(options.top_algorithm)),
                              top_files, matched_files.size(), matched_bytes);
}

void Application::RunDuplicateMode(std::span<const FileInfo> matched_files, const cli::CommandLineOptions& options) {
    analysis::DuplicateDetector duplicate_detector(scan::ResolveThreadCount(options.scan.thread_count));
    duplicate_detector.SetProgressCallback([this](const analysis::HashProgress& progress) {
        renderer_.RenderHashProgress(progress);  //call filesearch::render::ConsoleRenderer::RenderHashProgress
    });
    const analysis::DuplicateReport report = duplicate_detector.FindDuplicates(matched_files);
    renderer_.RenderDuplicateReport(report, options.display_limit);
}

}  // namespace filesearch::app
