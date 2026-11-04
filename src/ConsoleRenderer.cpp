#include "filesearch/ConsoleRenderer.hpp"

#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <format>
#include <iomanip>
#include <sstream>
#include <utility>

namespace filesearch::render {

namespace {

constexpr std::size_t kRankWidth = 4;
constexpr std::size_t kSizeWidth = 10;
constexpr std::size_t kTimeWidth = 16;
constexpr std::size_t kExtensionWidth = 10;
constexpr std::size_t kCountWidth = 10;
constexpr std::size_t kPercentWidth = 7;
constexpr std::size_t kBarWidth = 24;
constexpr std::size_t kRuleWidth = 78;
constexpr std::size_t kDefaultErrorPreview = 5;

// 常见的双宽字符区间（与 wcwidth 的东亚宽字符判定大致一致）
constexpr std::array<std::pair<char32_t, char32_t>, 12> kWideRanges{{
    {0x1100, 0x115F},    // 谚文字母
    {0x2E80, 0x303E},    // 中日韩部首、标点
    {0x3041, 0x33FF},    // 平假名、片假名、中日韩符号
    {0x3400, 0x4DBF},    // 中日韩统一表意文字扩展 A
    {0x4E00, 0x9FFF},    // 中日韩统一表意文字
    {0xA000, 0xA4CF},    // 彝文
    {0xAC00, 0xD7A3},    // 谚文音节
    {0xF900, 0xFAFF},    // 中日韩兼容表意文字
    {0xFE30, 0xFE4F},    // 中日韩兼容形式
    {0xFF00, 0xFF60},    // 全角 ASCII
    {0x1F300, 0x1FAFF},  // emoji
    {0x20000, 0x3FFFD},  // 中日韩扩展 B 及以后
}};

bool IsWideCodePoint(char32_t code_point) {
    return std::any_of(kWideRanges.begin(), kWideRanges.end(), [code_point](const std::pair<char32_t, char32_t>& range) {
        return code_point >= range.first && code_point <= range.second;
    });  //call 上面的 lambda（std::any_of 逐个区间判断）
}

// 只在第一次调用时查询本地时区；运行库没有时区数据时 current_zone() 会抛异常，此时退回 UTC
const std::chrono::time_zone* LocalTimeZone() {
    static const std::chrono::time_zone* const local_zone = []() -> const std::chrono::time_zone* {
        try {
            return std::chrono::current_zone();
        } catch (const std::exception&) {
            return nullptr;
        }
    }();  //call 上面的 lambda（局部静态变量初始化只执行一次，且 C++11 起是线程安全的）
    return local_zone;
}

}  // namespace

ConsoleRenderer::ConsoleRenderer(std::ostream& output, std::ostream& diagnostics)
    : output_(output), diagnostics_(diagnostics) {}

std::string ConsoleRenderer::FormatBytes(std::uintmax_t bytes) {
    constexpr std::array<std::string_view, 6> kUnits{"B", "KB", "MB", "GB", "TB", "PB"};
    std::size_t unit_index = 0;
    long double value = static_cast<long double>(bytes);
    while (value >= 1024.0L && unit_index + 1 < kUnits.size()) {
        value /= 1024.0L;
        ++unit_index;
    }
    std::ostringstream stream;
    if (unit_index == 0) {
        stream << bytes << ' ' << kUnits[unit_index];
    } else {
        stream << std::fixed << std::setprecision(2) << value << ' ' << kUnits[unit_index];
    }
    return stream.str();
}

std::string ConsoleRenderer::FormatCount(std::uintmax_t count) {
    const std::string digits = std::to_string(count);
    std::string grouped_digits;
    grouped_digits.reserve(digits.size() + digits.size() / 3);
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index != 0 && (digits.size() - index) % 3 == 0) {
            grouped_digits.push_back(',');
        }
        grouped_digits.push_back(digits[index]);
    }
    return grouped_digits;
}

std::string ConsoleRenderer::FormatDuration(std::chrono::nanoseconds duration) {
    const double seconds = std::chrono::duration<double>(duration).count();
    std::ostringstream stream;
    stream << std::fixed;
    if (seconds >= 1.0) {
        stream << std::setprecision(2) << seconds << " s";
    } else if (seconds >= 1e-3) {
        stream << std::setprecision(1) << seconds * 1e3 << " ms";
    } else {
        stream << std::setprecision(0) << seconds * 1e6 << " us";
    }
    return stream.str();
}

std::string ConsoleRenderer::FormatTime(std::filesystem::file_time_type file_time) {
    // file_clock -> system_clock（C++20 clock_cast），再截断到秒
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(file_time);
    const auto seconds_time = std::chrono::floor<std::chrono::seconds>(system_time);
    if (const std::chrono::time_zone* local_zone = LocalTimeZone()) {
        return std::format("{:%Y-%m-%d %H:%M}", std::chrono::zoned_time{local_zone, seconds_time});
    }
    return std::format("{:%Y-%m-%d %H:%M}", seconds_time);
}

std::size_t ConsoleRenderer::DisplayWidth(std::string_view utf8_text) {
    std::size_t width = 0;
    std::size_t index = 0;
    while (index < utf8_text.size()) {
        const auto lead_byte = static_cast<unsigned char>(utf8_text[index]);
        std::size_t sequence_length = 1;
        char32_t code_point = lead_byte;
        if ((lead_byte & 0xE0U) == 0xC0U) {
            sequence_length = 2;
            code_point = lead_byte & 0x1FU;
        } else if ((lead_byte & 0xF0U) == 0xE0U) {
            sequence_length = 3;
            code_point = lead_byte & 0x0FU;
        } else if ((lead_byte & 0xF8U) == 0xF0U) {
            sequence_length = 4;
            code_point = lead_byte & 0x07U;
        }
        if (index + sequence_length > utf8_text.size()) {
            return width + 1;  // 截断的多字节序列，按 1 列计
        }
        for (std::size_t offset = 1; offset < sequence_length; ++offset) {
            code_point = (code_point << 6) | (static_cast<unsigned char>(utf8_text[index + offset]) & 0x3FU);
        }
        width += IsWideCodePoint(code_point) ? 2 : 1;
        index += sequence_length;
    }
    return width;
}

std::string ConsoleRenderer::PadToWidth(std::string_view utf8_text, std::size_t width, Alignment alignment) {
    const std::size_t text_width = DisplayWidth(utf8_text);
    if (text_width >= width) {
        return std::string(utf8_text);
    }
    const std::string padding(width - text_width, ' ');
    return alignment == Alignment::Left ? std::string(utf8_text) + padding : padding + std::string(utf8_text);
}

void ConsoleRenderer::WriteProgressLine(const std::string& line, bool finished) {
    const std::size_t line_width = DisplayWidth(line);
    diagnostics_ << '\r' << line;
    if (line_width < progress_line_width_) {
        diagnostics_ << std::string(progress_line_width_ - line_width, ' ');  // 盖住上一次更长的内容
    }
    progress_line_width_ = finished ? 0 : std::max(progress_line_width_, line_width);
    if (finished) {
        diagnostics_ << '\n';
    }
    diagnostics_ << std::flush;
}

void ConsoleRenderer::RenderScanProgress(const scan::ScanProgress& progress) {
    constexpr std::string_view kSpinnerFrames = R"(|/-\)";
    const char spinner = progress.finished ? '*' : kSpinnerFrames[spinner_index_++ % kSpinnerFrames.size()];
    std::ostringstream line;
    line << spinner << " 已扫描 " << FormatCount(progress.scanned_files) << " 个文件 | "
         << FormatCount(progress.scanned_directories) << " 个目录 | " << FormatBytes(progress.scanned_bytes)
         << " | 错误 " << FormatCount(progress.error_count) << " | " << FormatDuration(progress.elapsed);
    WriteProgressLine(line.str(), progress.finished);
}

void ConsoleRenderer::RenderHashProgress(const analysis::HashProgress& progress) {
    const double percentage =
        progress.total_files == 0 ? 100.0 : 100.0 * static_cast<double>(progress.hashed_files) /
                                                static_cast<double>(progress.total_files);
    std::ostringstream line;
    line << (progress.finished ? '*' : '#') << " 哈希比对 " << FormatCount(progress.hashed_files) << " / "
         << FormatCount(progress.total_files) << " 个文件 (" << std::fixed << std::setprecision(1) << percentage
         << "%) | 已读取 " << FormatBytes(progress.hashed_bytes);
    WriteProgressLine(line.str(), progress.finished);
}

void ConsoleRenderer::RenderTitle(std::string_view title) {
    output_ << "\n== " << title << " ==\n";
}

void ConsoleRenderer::RenderRule(std::size_t width) {
    const StreamFormatGuard format_guard(output_);
    // setfill + setw 输出一串重复字符：这是 iomanip 画分隔线的经典写法
    output_ << std::setfill('-') << std::setw(static_cast<int>(width)) << "" << '\n';
}

void ConsoleRenderer::RenderScanSummary(const scan::ScanResult& scan_result, bool verbose) {
    output_ << "扫描完成: " << FormatCount(scan_result.files.size()) << " 个文件, "
            << FormatCount(scan_result.directory_count) << " 个目录, " << FormatBytes(scan_result.total_bytes)
            << ", 用时 " << FormatDuration(scan_result.elapsed) << " (" << scan_result.thread_count << " 线程, "
            << scan::ToString(scan_result.strategy) << ")\n";
    if (scan_result.error_count == 0) {
        return;
    }
    const std::size_t stored_count = scan_result.error_messages.size();
    const std::size_t preview_count = verbose ? stored_count : std::min(kDefaultErrorPreview, stored_count);
    diagnostics_ << "跳过 " << FormatCount(scan_result.error_count) << " 处无法访问的条目";
    if (preview_count < scan_result.error_count) {
        diagnostics_ << "（显示前 " << preview_count << " 条" << (verbose ? "" : "，-v 查看更多") << "）";
    }
    diagnostics_ << ":\n";
    for (std::size_t index = 0; index < preview_count; ++index) {
        diagnostics_ << "  " << scan_result.error_messages[index] << '\n';
    }
}

void ConsoleRenderer::RenderFilterSummary(std::size_t matched_count, std::uintmax_t matched_bytes,
                                          std::size_t scanned_count) {
    output_ << "筛选后: " << FormatCount(matched_count) << " / " << FormatCount(scanned_count) << " 个文件, "
            << FormatBytes(matched_bytes) << '\n';
}

void ConsoleRenderer::RenderFileTable(std::string_view title, std::span<const FileInfo> files, std::size_t total_count,
                                      std::uintmax_t total_bytes) {
    RenderTitle(title);
    if (files.empty()) {
        output_ << "  （没有匹配的文件）\n";
        return;
    }
    const StreamFormatGuard format_guard(output_);
    output_ << PadToWidth("#", kRankWidth, Alignment::Right) << "  " << PadToWidth("大小", kSizeWidth, Alignment::Right)
            << "  " << PadToWidth("修改时间", kTimeWidth, Alignment::Left) << "  "
            << PadToWidth("后缀", kExtensionWidth, Alignment::Left) << "  路径\n";
    RenderRule(kRuleWidth);

    std::size_t rank = 0;
    for (const FileInfo& file : files) {
        ++rank;
        // 纯 ASCII 的列直接用 std::setw；可能含中文的列用 PadToWidth 按显示宽度补齐
        output_ << std::right << std::setw(static_cast<int>(kRankWidth)) << rank << "  "
                << std::setw(static_cast<int>(kSizeWidth)) << FormatBytes(file.size) << "  " << std::left
                << std::setw(static_cast<int>(kTimeWidth)) << FormatTime(file.last_write_time) << "  "
                << PadToWidth(file.extension.empty() ? "-" : file.extension, kExtensionWidth, Alignment::Left)
                << "  " << text::PathToUtf8(file.path) << '\n';
    }
    output_ << "显示 " << FormatCount(files.size()) << " / " << FormatCount(total_count) << " 个文件, 合计 "
            << FormatBytes(total_bytes) << '\n';
}

void ConsoleRenderer::RenderExtensionStatistics(std::span<const analysis::ExtensionStatistics> statistics,
                                                std::uintmax_t total_bytes, std::size_t row_limit) {
    RenderTitle("后缀空间占用");
    if (statistics.empty()) {
        output_ << "  （没有文件）\n";
        return;
    }
    const StreamFormatGuard format_guard(output_);
    output_ << PadToWidth("后缀", kExtensionWidth, Alignment::Left) << "  "
            << PadToWidth("文件数", kCountWidth, Alignment::Right) << "  "
            << PadToWidth("总大小", kSizeWidth, Alignment::Right) << "  "
            << PadToWidth("占比", kPercentWidth, Alignment::Right) << "  分布\n";
    RenderRule(kRuleWidth);

    const auto render_row = [&](std::string_view label, std::size_t file_count, std::uintmax_t bytes) {
        const double share = total_bytes == 0 ? 0.0 : static_cast<double>(bytes) / static_cast<double>(total_bytes);
        const auto bar_length = static_cast<std::size_t>(std::lround(share * static_cast<double>(kBarWidth)));
        output_ << PadToWidth(label, kExtensionWidth, Alignment::Left) << "  " << std::right
                << std::setw(static_cast<int>(kCountWidth)) << FormatCount(file_count) << "  "
                << std::setw(static_cast<int>(kSizeWidth)) << FormatBytes(bytes) << "  " << std::fixed
                << std::setprecision(1) << std::setw(static_cast<int>(kPercentWidth) - 1) << share * 100.0 << '%'
                << "  " << std::string(bar_length, '#') << '\n';
    };

    const std::size_t shown_count = row_limit == 0 ? statistics.size() : std::min(row_limit, statistics.size());
    for (const analysis::ExtensionStatistics& entry : statistics.first(shown_count)) {
        render_row(entry.extension.empty() ? "(无后缀)" : entry.extension, entry.file_count,
                   entry.total_bytes);  //call 上面的 render_row lambda
    }
    if (shown_count < statistics.size()) {
        // 超出 --limit 的后缀合并成一行“其他”
        std::size_t other_file_count = 0;
        std::uintmax_t other_bytes = 0;
        for (const analysis::ExtensionStatistics& entry : statistics.subspan(shown_count)) {
            other_file_count += entry.file_count;
            other_bytes += entry.total_bytes;
        }
        render_row("(其他)", other_file_count, other_bytes);  //call 上面的 render_row lambda
    }
    output_ << "共 " << statistics.size() << " 种后缀, 合计 " << FormatBytes(total_bytes) << '\n';
}

void ConsoleRenderer::RenderDuplicateReport(const analysis::DuplicateReport& report, std::size_t group_limit) {
    RenderTitle("重复文件（大小与内容哈希均相同）");
    std::uintmax_t reclaimable_bytes = 0;
    std::size_t duplicate_file_count = 0;
    for (const analysis::DuplicateGroup& group : report.groups) {
        reclaimable_bytes += group.ReclaimableBytes();
        duplicate_file_count += group.files.size();
    }
    if (report.groups.empty()) {
        output_ << "  （没有发现重复文件）\n";
    }

    const StreamFormatGuard format_guard(output_);
    const std::size_t shown_count = group_limit == 0 ? report.groups.size() : std::min(group_limit, report.groups.size());
    for (std::size_t group_index = 0; group_index < shown_count; ++group_index) {
        const analysis::DuplicateGroup& group = report.groups[group_index];
        output_ << '[' << group_index + 1 << "] " << FormatBytes(group.file_size) << " x " << group.files.size()
                << " 个, 可释放 " << FormatBytes(group.ReclaimableBytes()) << "  hash=" << std::hex
                << std::setfill('0') << std::setw(16) << group.content_hash << std::dec << std::setfill(' ') << '\n';
        for (const FileInfo& file : group.files) {
            output_ << "      " << text::PathToUtf8(file.path) << '\n';
        }
    }
    if (shown_count < report.groups.size()) {
        output_ << "... 还有 " << report.groups.size() - shown_count << " 组未显示（--limit 0 显示全部）\n";
    }
    output_ << "共 " << report.groups.size() << " 组重复 (" << FormatCount(duplicate_file_count)
            << " 个文件), 每组只保留一份可释放 " << FormatBytes(reclaimable_bytes) << '\n'
            << "候选 " << FormatCount(report.candidate_file_count) << " 个文件, 读取 "
            << FormatBytes(report.hashed_bytes) << ", 无法读取 " << report.unreadable_file_count << " 个\n";
}

}  // namespace filesearch::render
