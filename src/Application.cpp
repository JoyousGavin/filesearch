#include "filesearch/Application.hpp"

#include "filesearch/CacheStore.hpp"
#include "filesearch/DuplicateDetector.hpp"
#include "filesearch/FileAnalyzer.hpp"
#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace filesearch::app {

namespace {

constexpr int kBenchmarkRepetitionCount = 5;

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

Application::Application(std::ostream& output, std::ostream& diagnostics, timing::TimingReport& timing_report)
    : diagnostics_(diagnostics), renderer_(output, diagnostics), timing_report_(timing_report) {}

int Application::Run(const cli::CommandLineOptions& options) {
    scan::ScanOptions scan_options = options.scan;
    scan_options.root_directory = scan::NormalizeRootDirectory(options.scan.root_directory);
    std::error_code status_error;
    if (!std::filesystem::is_directory(scan_options.root_directory, status_error)) {
        diagnostics_ << "不是有效目录: " << text::PathToUtf8(scan_options.root_directory) << '\n';
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

    scan::ScanResult scan_result = ScanWithCache(std::move(scan_options), options);

    // 模块 3：过滤。没有筛选条件时直接接管扫描结果，省掉一次全量拷贝
    const std::size_t scanned_count = scan_result.files.size();
    std::vector<FileInfo> matched_files;
    {
        const timing::ScopedTimer filter_timer(timing_report_, "3 数据过滤");
        if (file_filter->HasConditions()) {
            matched_files = file_filter->ApplyWithRanges(scan_result.files);
        } else {
            matched_files = std::move(scan_result.files);
        }
    }
    const std::uintmax_t matched_bytes = analysis::FileAnalyzer::SumBytes(matched_files);
    if (file_filter->HasConditions()) {
        renderer_.RenderFilterSummary(matched_files.size(), matched_bytes, scanned_count);
    }
    // 基准测试要在全部文件上比较过滤实现；没有筛选条件时 matched_files 就是全部文件
    const std::span<const FileInfo> all_files =
        file_filter->HasConditions() ? std::span<const FileInfo>(scan_result.files) : std::span<const FileInfo>(matched_files);

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
            case cli::RunMode::Benchmark:
                RunBenchmarkMode(all_files, matched_files, *file_filter, options);
                break;
        }
    }

    if (options.show_timing) {
        renderer_.RenderTimingReport(timing_report_);
    }
    return kExitSuccess;
}

scan::ScanResult Application::ScanWithCache(scan::ScanOptions scan_options, const cli::CommandLineOptions& options) {
    const std::filesystem::path root_directory = scan_options.root_directory;

    // 模块 5：读取缓存。cache_index 必须活到扫描结束，因为 scan_options.previous_snapshot 指向它
    std::optional<cache::CacheStore> cache_store;
    std::optional<cache::CacheIndex> cache_index;
    if (options.cache_enabled) {
        cache_store.emplace(options.cache_file.value_or(cache::CacheStore::DefaultCacheFile(root_directory)));
        scan_options.record_directories = true;
        if (options.refresh_cache) {
            renderer_.RenderCacheLoad(cache::LoadResult{cache::LoadStatus::Skipped, std::nullopt, ""},
                                      cache_store->CacheFile());
        } else {
            const timing::ScopedTimer load_timer(timing_report_, "5 缓存读取");
            cache::LoadResult load_result = cache_store->Load(root_directory);
            renderer_.RenderCacheLoad(load_result, cache_store->CacheFile());
            if (load_result.index) {
                cache_index = std::move(load_result.index);
                scan_options.previous_snapshot = &*cache_index;
            } else if (load_result.status == cache::LoadStatus::ForeignFile) {
                cache_store.reset();  // 不是本程序的文件：本次彻底不碰它
                scan_options.record_directories = false;
            }
        }
    }

    // 模块 2：扫描
    scan::ScanResult scan_result;
    {
        const timing::ScopedTimer scan_timer(timing_report_, "2 扫描引擎");
        scan::ScanEngine scan_engine(std::move(scan_options));
        scan_engine.SetProgressCallback([this](const scan::ScanProgress& progress) {
            renderer_.RenderScanProgress(progress);  //call filesearch::render::ConsoleRenderer::RenderScanProgress
        });
        scan_result = scan_engine.Run();
    }
    cache_index.reset();  // 快照只在扫描期间使用，尽早释放内存
    renderer_.RenderScanSummary(scan_result, options.verbose);

    // 每个访问到的目录都命中了快照 => 没有任何目录增删条目，缓存内容不会变，省掉一次重写
    const bool cache_up_to_date = scan_result.reused_directory_count > 0 &&
                                  scan_result.reused_directory_count == scan_result.directories.size();
    if (cache_store && cache_up_to_date) {
        renderer_.RenderCacheUpToDate(cache_store->CacheFile());
        cache_store.reset();
    }

    // 模块 5：写入缓存（必须在过滤阶段移走 files 之前）
    if (cache_store) {
        const timing::ScopedTimer save_timer(timing_report_, "5 缓存写入");
        if (const std::optional<std::string> save_error = cache_store->Save(root_directory, scan_result)) {
            diagnostics_ << "缓存写入失败: " << *save_error << '\n';
        } else {
            renderer_.RenderCacheSaved(cache_store->CacheFile(), scan_result);
        }
    }
    return scan_result;
}

void Application::RunSearchMode(std::vector<FileInfo>& matched_files, std::uintmax_t matched_bytes,
                                const cli::CommandLineOptions& options) {
    {
        const timing::ScopedTimer sort_timer(timing_report_, "3 搜索排序");
        filter::FileFilter::Sort(matched_files, options.sort_key);
    }
    const timing::ScopedTimer render_timer(timing_report_, "4 输出渲染");
    const std::span<const FileInfo> shown_files =
        std::span<const FileInfo>(matched_files).first(ApplyLimit(matched_files.size(), options.display_limit));
    renderer_.RenderFileTable(std::format("搜索结果（{}）", DescribeSortKey(options.sort_key)), shown_files,
                              matched_files.size(), matched_bytes);
}

void Application::RunStatisticsMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                                    const cli::CommandLineOptions& options) {
    std::vector<analysis::ExtensionStatistics> statistics;
    {
        const timing::ScopedTimer statistics_timer(timing_report_, "3 后缀统计");
        statistics = analysis::FileAnalyzer::ComputeExtensionStatistics(matched_files);
    }
    const timing::ScopedTimer render_timer(timing_report_, "4 输出渲染");
    renderer_.RenderExtensionStatistics(statistics, matched_bytes, options.display_limit);
}

void Application::RunTopMode(std::span<const FileInfo> matched_files, std::uintmax_t matched_bytes,
                             const cli::CommandLineOptions& options) {
    std::vector<FileInfo> top_files;
    {
        const timing::ScopedTimer top_timer(timing_report_, "3 Top K");
        top_files = analysis::FileAnalyzer::TopLargest(matched_files, options.top_count, options.top_algorithm);
    }
    const timing::ScopedTimer render_timer(timing_report_, "4 输出渲染");
    renderer_.RenderFileTable(std::format("最大的 {} 个文件（{}）", options.top_count,
                                          analysis::ToString(options.top_algorithm)),
                              top_files, matched_files.size(), matched_bytes);
}

void Application::RunDuplicateMode(std::span<const FileInfo> matched_files, const cli::CommandLineOptions& options) {
    analysis::DuplicateReport report;
    {
        const timing::ScopedTimer duplicate_timer(timing_report_, "3 重复检测");
        analysis::DuplicateDetector duplicate_detector(scan::ResolveThreadCount(options.scan.thread_count));
        duplicate_detector.SetProgressCallback([this](const analysis::HashProgress& progress) {
            renderer_.RenderHashProgress(progress);  //call filesearch::render::ConsoleRenderer::RenderHashProgress
        });
        report = duplicate_detector.FindDuplicates(matched_files);
    }
    const timing::ScopedTimer render_timer(timing_report_, "4 输出渲染");
    renderer_.RenderDuplicateReport(report, options.display_limit);
}

void Application::RunBenchmarkMode(std::span<const FileInfo> all_files, std::span<const FileInfo> matched_files,
                                   const filter::FileFilter& file_filter, const cli::CommandLineOptions& options) {
    std::vector<timing::BenchmarkResult> results;
    // 每个操作运行多次，记录最快值与平均值；operation 返回结果元素个数
    const auto measure = [&results](std::string operation_name, const auto& operation) {
        timing::BenchmarkResult result;
        result.operation_name = std::move(operation_name);
        result.fastest_duration = std::chrono::nanoseconds::max();
        std::chrono::nanoseconds total_duration{0};
        for (int repetition = 0; repetition < kBenchmarkRepetitionCount; ++repetition) {
            const std::chrono::nanoseconds duration = timing::MeasureCall([&] {
                result.result_size = operation();  //call 下方 measure 调用处传入的 operation lambda
            });  //call filesearch::timing::MeasureCall（内部 std::invoke 上面的 lambda）
            result.fastest_duration = std::min(result.fastest_duration, duration);
            total_duration += duration;
        }
        result.average_duration = total_duration / kBenchmarkRepetitionCount;
        results.push_back(std::move(result));
    };

    measure("过滤 std::copy_if", [&] {
        return file_filter.Apply(all_files).size();  //call filesearch::filter::FileFilter::Apply
    });  //call 上面的 measure lambda
    measure("过滤 views::filter + ranges::copy", [&] {
        return file_filter.ApplyWithRanges(all_files).size();  //call filesearch::filter::FileFilter::ApplyWithRanges
    });  //call 上面的 measure lambda
    for (const analysis::TopAlgorithm top_algorithm :
         {analysis::TopAlgorithm::Heap, analysis::TopAlgorithm::PartialSortCopy, analysis::TopAlgorithm::NthElement,
          analysis::TopAlgorithm::Ranges, analysis::TopAlgorithm::FullSort}) {
        measure(std::format("Top {} {}", options.top_count, analysis::ToString(top_algorithm)), [&] {
            return analysis::FileAnalyzer::TopLargest(matched_files, options.top_count, top_algorithm).size();  //call filesearch::analysis::FileAnalyzer::TopLargest
        });  //call 上面的 measure lambda
    }
    measure("后缀统计 unordered_map", [&] {
        return analysis::FileAnalyzer::ComputeExtensionStatistics(matched_files).size();  //call filesearch::analysis::FileAnalyzer::ComputeExtensionStatistics
    });  //call 上面的 measure lambda

    const timing::ScopedTimer render_timer(timing_report_, "4 输出渲染");
    renderer_.RenderBenchmark(results, all_files.size(), kBenchmarkRepetitionCount);
}

}  // namespace filesearch::app
