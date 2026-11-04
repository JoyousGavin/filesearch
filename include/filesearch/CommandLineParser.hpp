#pragma once

#include "filesearch/FileAnalyzer.hpp"
#include "filesearch/FileFilter.hpp"
#include "filesearch/ScanEngine.hpp"
#include "filesearch/TextUtilities.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace filesearch::cli {

// 选项表中的一行：既用于解析，也用于生成帮助文本
struct OptionSpec {
    std::string_view long_name;    // 规范名，同时是参数表 unordered_map 的键，如 "directory"
    char short_name = '\0';        // 短选项字母，'\0' 表示没有
    bool takes_value = false;
    bool accumulates = false;      // 重复出现时用逗号拼接（如 -e .mp4 -e .mkv），否则后者覆盖前者
    std::string_view value_hint;
    std::string_view description;
};

enum class RunMode {
    Search,      // 过滤 + 排序后列出
    Statistics,  // 后缀空间占用统计
    TopFiles,    // 最大的 K 个文件
    Duplicates,  // 重复文件检测
    Benchmark,   // 用 std::chrono 对比各过滤 / Top K 实现的耗时
};

// 解析并校验后的强类型参数
struct CommandLineOptions {
    scan::ScanOptions scan;
    filter::FilterCriteria criteria;
    std::vector<RunMode> modes{RunMode::Search};  // 按给出的顺序依次执行
    filter::SortKey sort_key = filter::SortKey::Size;
    std::size_t display_limit = 50;  // 每张表最多显示的行数（重复检测为组数），0 表示不限制
    std::size_t top_count = 10;
    analysis::TopAlgorithm top_algorithm = analysis::TopAlgorithm::Heap;
    bool verbose = false;
    bool show_timing = false;
    bool cache_enabled = false;
    bool refresh_cache = false;                       // 忽略旧缓存，完整扫描后重写
    std::optional<std::filesystem::path> cache_file;  // 未指定时使用 CacheStore::DefaultCacheFile
};

struct HelpRequest {};

struct ParseError {
    std::string message;
};

using ParseOutcome = std::variant<CommandLineOptions, HelpRequest, ParseError>;

// 手写状态机解析命令行：
//   ExpectToken    --(-x / --name，需要参数值)-->  ExpectValue
//   ExpectValue    --(任意 token 作为参数值)----->  ExpectToken
//   ExpectToken    --("--")------------------------>  PositionalOnly（之后全部视为位置参数）
// 支持 -d value、-dvalue、--directory value、--directory=value、组合短开关 -hx
class CommandLineParser {
public:
    CommandLineParser();

    // arguments[0] 是程序名
    ParseOutcome Parse(const std::vector<std::string>& arguments);

    static void PrintUsage(std::ostream& output);

    // "100MB" / "1.5G" / "512k" / "4096" -> 字节数（按 1024 进制）；格式非法或溢出时返回 std::nullopt
    static std::optional<std::uintmax_t> ParseSize(std::string_view text);
    static std::optional<std::size_t> ParseCount(std::string_view text);

private:
    enum class State {
        ExpectToken,
        ExpectValue,
        PositionalOnly,
    };

    // 下面几个函数返回错误信息；std::nullopt 表示成功
    std::optional<std::string> ConsumeToken(std::string_view token);
    std::optional<std::string> HandleLongOption(std::string_view option_body);
    std::optional<std::string> HandleShortOptions(std::string_view option_body);
    void BeginOption(const OptionSpec& option_spec);
    void StoreValue(const OptionSpec& option_spec, std::string_view value);

    std::optional<std::string_view> FindValue(std::string_view long_name) const;
    ParseOutcome BuildOptions() const;

    std::unordered_map<std::string_view, const OptionSpec*> long_options_;
    std::unordered_map<char, const OptionSpec*> short_options_;

    // 原始参数表：规范长选项名 -> 原始字符串值（开关型选项的值为 "true"）
    std::unordered_map<std::string, std::string, text::TransparentStringHash, std::equal_to<>> parsed_values_;
    std::vector<std::string> positional_arguments_;

    State state_ = State::ExpectToken;
    const OptionSpec* pending_option_ = nullptr;
};

}  // namespace filesearch::cli
