#include "filesearch/CommandLineParser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <system_error>
#include <utility>

namespace filesearch::cli {

namespace {

constexpr std::array kOptionSpecs{
    OptionSpec{"mode", 'm', true, true, "<search|stats|top|dup>", "运行模式，可用逗号组合，如 stats,top（默认 search）"},
    OptionSpec{"directory", 'd', true, false, "<path>", "要扫描的根目录（默认当前目录，也可作为位置参数）"},
    OptionSpec{"extension", 'e', true, true, "<list>", "只保留这些后缀，逗号分隔，可重复，如 .mp4,.mkv"},
    OptionSpec{"min-size", 's', true, false, "<size>", "最小文件大小，如 100MB、1.5G、512K"},
    OptionSpec{"max-size", 'S', true, false, "<size>", "最大文件大小"},
    OptionSpec{"name", 'n', true, false, "<text>", "文件名包含该文本（不区分大小写）"},
    OptionSpec{"regex", 'r', true, false, "<pattern>", "文件名匹配该正则（ECMAScript，不区分大小写）"},
    OptionSpec{"sort", '\0', true, false, "<size|path|time>", "搜索结果排序：大小降序 / 路径升序 / 修改时间降序"},
    OptionSpec{"limit", 'l', true, false, "<n>", "每张表最多显示多少行/组（默认 50，0 表示全部）"},
    OptionSpec{"top", 'k', true, false, "<n>", "top 模式显示最大的 N 个文件（默认 10）"},
    OptionSpec{"top-algorithm", '\0', true, false, "<heap|partial|nth|sort>", "Top K 算法（默认 heap：小顶堆 O(N log K)）"},
    OptionSpec{"threads", 't', true, false, "<n>", "扫描线程数（默认 CPU 逻辑核心数）"},
    OptionSpec{"strategy", '\0', true, false, "<queue|async>", "并发策略：queue 共享目录队列（默认）/ async 每个子文件夹一个任务"},
    OptionSpec{"verbose", 'v', false, false, "", "显示全部扫描错误明细"},
    OptionSpec{"help", 'h', false, false, "", "显示帮助"},
};

template <typename Enumeration>
struct Choice {
    std::string_view name;
    Enumeration value;
};

constexpr std::array kStrategyChoices{
    Choice<scan::ScanStrategy>{"queue", scan::ScanStrategy::WorkQueue},
    Choice<scan::ScanStrategy>{"async", scan::ScanStrategy::AsyncPerSubdirectory},
};

constexpr std::array kModeChoices{
    Choice<RunMode>{"search", RunMode::Search},
    Choice<RunMode>{"stats", RunMode::Statistics},
    Choice<RunMode>{"top", RunMode::TopFiles},
    Choice<RunMode>{"dup", RunMode::Duplicates},
};

constexpr std::array kTopAlgorithmChoices{
    Choice<analysis::TopAlgorithm>{"heap", analysis::TopAlgorithm::Heap},
    Choice<analysis::TopAlgorithm>{"partial", analysis::TopAlgorithm::PartialSortCopy},
    Choice<analysis::TopAlgorithm>{"nth", analysis::TopAlgorithm::NthElement},
    Choice<analysis::TopAlgorithm>{"sort", analysis::TopAlgorithm::FullSort},
};

constexpr std::array kSortChoices{
    Choice<filter::SortKey>{"size", filter::SortKey::Size},
    Choice<filter::SortKey>{"path", filter::SortKey::Path},
    Choice<filter::SortKey>{"time", filter::SortKey::Time},
};

template <typename Enumeration, std::size_t ChoiceCount>
std::optional<Enumeration> LookupChoice(const std::array<Choice<Enumeration>, ChoiceCount>& choices,
                                        std::string_view name) {
    const std::string lowered_name = text::ToLowerAscii(name);
    const auto found = std::find_if(choices.begin(), choices.end(), [&lowered_name](const Choice<Enumeration>& choice) {
        return choice.name == lowered_name;
    });  //call 上面的 lambda（std::find_if 逐个比较选项名）
    if (found == choices.end()) {
        return std::nullopt;
    }
    return found->value;
}

std::string QuoteOption(std::string_view long_name) {
    return "--" + std::string(long_name);
}

}  // namespace

CommandLineParser::CommandLineParser() {
    for (const OptionSpec& option_spec : kOptionSpecs) {
        long_options_.emplace(option_spec.long_name, &option_spec);
        if (option_spec.short_name != '\0') {
            short_options_.emplace(option_spec.short_name, &option_spec);
        }
    }
}

ParseOutcome CommandLineParser::Parse(const std::vector<std::string>& arguments) {
    state_ = State::ExpectToken;
    pending_option_ = nullptr;
    parsed_values_.clear();
    positional_arguments_.clear();

    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (std::optional<std::string> error_message = ConsumeToken(arguments[index])) {
            return ParseError{std::move(*error_message)};
        }
    }
    if (state_ == State::ExpectValue) {
        return ParseError{"选项 " + QuoteOption(pending_option_->long_name) + " 缺少参数值"};
    }
    if (parsed_values_.contains(std::string_view{"help"})) {
        return HelpRequest{};
    }
    return BuildOptions();
}

std::optional<std::string> CommandLineParser::ConsumeToken(std::string_view token) {
    switch (state_) {
        case State::ExpectValue:
            // 上一个选项需要参数值：无论 token 长什么样（哪怕以 '-' 开头）都当作值
            StoreValue(*pending_option_, token);
            pending_option_ = nullptr;
            state_ = State::ExpectToken;
            return std::nullopt;
        case State::PositionalOnly:
            positional_arguments_.emplace_back(token);
            return std::nullopt;
        case State::ExpectToken:
            break;
    }

    if (token == "--") {
        state_ = State::PositionalOnly;
        return std::nullopt;
    }
    if (token.starts_with("--")) {
        return HandleLongOption(token.substr(2));
    }
    if (token.size() > 1 && token.front() == '-') {
        return HandleShortOptions(token.substr(1));
    }
    positional_arguments_.emplace_back(token);
    return std::nullopt;
}

std::optional<std::string> CommandLineParser::HandleLongOption(std::string_view option_body) {
    const std::size_t equal_position = option_body.find('=');
    const std::string_view long_name = option_body.substr(0, equal_position);
    const auto found = long_options_.find(long_name);
    if (found == long_options_.end()) {
        return "未知选项: " + QuoteOption(long_name);
    }
    const OptionSpec& option_spec = *found->second;
    if (equal_position == std::string_view::npos) {
        BeginOption(option_spec);
        return std::nullopt;
    }
    if (!option_spec.takes_value) {
        return "选项 " + QuoteOption(long_name) + " 不接受参数值";
    }
    StoreValue(option_spec, option_body.substr(equal_position + 1));
    return std::nullopt;
}

std::optional<std::string> CommandLineParser::HandleShortOptions(std::string_view option_body) {
    for (std::size_t index = 0; index < option_body.size(); ++index) {
        const auto found = short_options_.find(option_body[index]);
        if (found == short_options_.end()) {
            return "未知选项: -" + std::string(1, option_body[index]);
        }
        const OptionSpec& option_spec = *found->second;
        if (option_spec.takes_value) {
            // -t8：字母后面紧跟的部分就是值；-t 8：值在下一个 token
            const std::string_view attached_value = option_body.substr(index + 1);
            if (attached_value.empty()) {
                BeginOption(option_spec);
            } else {
                StoreValue(option_spec, attached_value);
            }
            return std::nullopt;
        }
        StoreValue(option_spec, "true");
    }
    return std::nullopt;
}

void CommandLineParser::BeginOption(const OptionSpec& option_spec) {
    if (option_spec.takes_value) {
        pending_option_ = &option_spec;
        state_ = State::ExpectValue;
    } else {
        StoreValue(option_spec, "true");
    }
}

void CommandLineParser::StoreValue(const OptionSpec& option_spec, std::string_view value) {
    const auto [iterator, inserted] = parsed_values_.try_emplace(std::string(option_spec.long_name), value);
    if (inserted) {
        return;
    }
    if (option_spec.accumulates) {
        iterator->second.append(",").append(value);
    } else {
        iterator->second.assign(value);
    }
}

std::optional<std::string_view> CommandLineParser::FindValue(std::string_view long_name) const {
    const auto found = parsed_values_.find(long_name);  // 透明哈希：用 string_view 直接查，不构造 std::string
    if (found == parsed_values_.end()) {
        return std::nullopt;
    }
    return std::string_view(found->second);
}

std::optional<std::uintmax_t> CommandLineParser::ParseSize(std::string_view text) {
    text = text::TrimWhitespace(text);
    const auto unit_begin = std::find_if(text.begin(), text.end(), [](char character) {
        return std::isdigit(static_cast<unsigned char>(character)) == 0 && character != '.';
    });  //call 上面的 lambda（找到第一个既不是数字也不是小数点的字符）
    const std::string_view number_part = text.substr(0, static_cast<std::size_t>(unit_begin - text.begin()));
    const std::string unit_part = text::ToLowerAscii(text::TrimWhitespace(text.substr(number_part.size())));
    if (number_part.empty()) {
        return std::nullopt;
    }

    double number = 0.0;
    const char* const number_end = number_part.data() + number_part.size();
    const auto [parse_end, parse_error] = std::from_chars(number_part.data(), number_end, number);
    if (parse_error != std::errc{} || parse_end != number_end) {
        return std::nullopt;
    }

    static const std::unordered_map<std::string_view, std::uintmax_t> kUnitMultipliers{
        {"", 1},
        {"b", 1},
        {"k", 1ULL << 10}, {"kb", 1ULL << 10}, {"kib", 1ULL << 10},
        {"m", 1ULL << 20}, {"mb", 1ULL << 20}, {"mib", 1ULL << 20},
        {"g", 1ULL << 30}, {"gb", 1ULL << 30}, {"gib", 1ULL << 30},
        {"t", 1ULL << 40}, {"tb", 1ULL << 40}, {"tib", 1ULL << 40},
    };
    const auto unit = kUnitMultipliers.find(unit_part);
    if (unit == kUnitMultipliers.end()) {
        return std::nullopt;
    }

    const long double bytes = std::round(static_cast<long double>(number) * static_cast<long double>(unit->second));
    if (bytes > static_cast<long double>(std::numeric_limits<std::uintmax_t>::max())) {
        return std::nullopt;
    }
    return static_cast<std::uintmax_t>(bytes);
}

std::optional<std::size_t> CommandLineParser::ParseCount(std::string_view text) {
    text = text::TrimWhitespace(text);
    std::size_t value = 0;
    const char* const text_end = text.data() + text.size();
    const auto [parse_end, parse_error] = std::from_chars(text.data(), text_end, value);
    if (text.empty() || parse_error != std::errc{} || parse_end != text_end) {
        return std::nullopt;
    }
    return value;
}

ParseOutcome CommandLineParser::BuildOptions() const {
    CommandLineOptions options;

    // 根目录：-d 优先，其次是第一个位置参数，都没有时用当前目录
    const std::optional<std::string_view> directory_value = FindValue("directory");
    const std::size_t allowed_positional_count = directory_value ? 0 : 1;
    if (positional_arguments_.size() > allowed_positional_count) {
        return ParseError{"多余的参数: " + positional_arguments_[allowed_positional_count]};
    }
    if (directory_value) {
        options.scan.root_directory = text::PathFromUtf8(*directory_value);
    } else if (!positional_arguments_.empty()) {
        options.scan.root_directory = text::PathFromUtf8(positional_arguments_.front());
    } else {
        std::error_code current_path_error;
        options.scan.root_directory = std::filesystem::current_path(current_path_error);
        if (current_path_error) {
            options.scan.root_directory = ".";
        }
    }

    if (const std::optional<std::string_view> extension_value = FindValue("extension")) {
        std::vector<std::string>& extensions = options.criteria.extensions;
        for (const std::string_view piece : text::SplitString(*extension_value, ',')) {
            const std::string_view trimmed_piece = text::TrimWhitespace(piece);
            if (trimmed_piece.empty()) {
                continue;
            }
            std::string extension = text::ToLowerAscii(trimmed_piece);
            if (extension.front() != '.') {
                extension.insert(extension.begin(), '.');
            }
            extensions.push_back(std::move(extension));
        }
        std::sort(extensions.begin(), extensions.end());
        extensions.erase(std::unique(extensions.begin(), extensions.end()), extensions.end());  // 去重
        if (extensions.empty()) {
            return ParseError{"--extension 没有给出有效的后缀"};
        }
    }

    if (const std::optional<std::string_view> size_value = FindValue("min-size")) {
        options.criteria.minimum_size = ParseSize(*size_value);
        if (!options.criteria.minimum_size) {
            return ParseError{"无法识别的大小: " + std::string(*size_value) + "（示例: 100MB、1.5G、512K）"};
        }
    }
    if (const std::optional<std::string_view> size_value = FindValue("max-size")) {
        options.criteria.maximum_size = ParseSize(*size_value);
        if (!options.criteria.maximum_size) {
            return ParseError{"无法识别的大小: " + std::string(*size_value) + "（示例: 100MB、1.5G、512K）"};
        }
    }
    if (options.criteria.minimum_size && options.criteria.maximum_size &&
        *options.criteria.minimum_size > *options.criteria.maximum_size) {
        return ParseError{"--min-size 不能大于 --max-size"};
    }

    if (const std::optional<std::string_view> name_value = FindValue("name")) {
        options.criteria.name_substring = std::string(*name_value);
    }
    if (const std::optional<std::string_view> regex_value = FindValue("regex")) {
        options.criteria.name_pattern = std::string(*regex_value);
    }

    if (const std::optional<std::string_view> threads_value = FindValue("threads")) {
        const std::optional<std::size_t> thread_count = ParseCount(*threads_value);
        if (!thread_count || *thread_count == 0) {
            return ParseError{"--threads 需要正整数，实际为: " + std::string(*threads_value)};
        }
        options.scan.thread_count = *thread_count;
    }
    if (const std::optional<std::string_view> strategy_value = FindValue("strategy")) {
        const std::optional<scan::ScanStrategy> strategy = LookupChoice(kStrategyChoices, *strategy_value);
        if (!strategy) {
            return ParseError{"--strategy 只能是 queue 或 async，实际为: " + std::string(*strategy_value)};
        }
        options.scan.strategy = *strategy;
    }
    if (const std::optional<std::string_view> sort_value = FindValue("sort")) {
        const std::optional<filter::SortKey> sort_key = LookupChoice(kSortChoices, *sort_value);
        if (!sort_key) {
            return ParseError{"--sort 只能是 size、path 或 time，实际为: " + std::string(*sort_value)};
        }
        options.sort_key = *sort_key;
    }
    if (const std::optional<std::string_view> limit_value = FindValue("limit")) {
        const std::optional<std::size_t> display_limit = ParseCount(*limit_value);
        if (!display_limit) {
            return ParseError{"--limit 需要非负整数，实际为: " + std::string(*limit_value)};
        }
        options.display_limit = *display_limit;
    }

    if (const std::optional<std::string_view> mode_value = FindValue("mode")) {
        options.modes.clear();
        for (const std::string_view piece : text::SplitString(*mode_value, ',')) {
            const std::string_view mode_name = text::TrimWhitespace(piece);
            const std::optional<RunMode> mode = LookupChoice(kModeChoices, mode_name);
            if (!mode) {
                return ParseError{"--mode 只能是 search、stats、top、dup 的组合，实际为: " + std::string(mode_name)};
            }
            if (std::find(options.modes.begin(), options.modes.end(), *mode) == options.modes.end()) {
                options.modes.push_back(*mode);  // 保持用户给出的顺序，重复的忽略
            }
        }
    }
    if (const std::optional<std::string_view> top_value = FindValue("top")) {
        const std::optional<std::size_t> top_count = ParseCount(*top_value);
        if (!top_count || *top_count == 0) {
            return ParseError{"--top 需要正整数，实际为: " + std::string(*top_value)};
        }
        options.top_count = *top_count;
    }
    if (const std::optional<std::string_view> algorithm_value = FindValue("top-algorithm")) {
        const std::optional<analysis::TopAlgorithm> top_algorithm =
            LookupChoice(kTopAlgorithmChoices, *algorithm_value);
        if (!top_algorithm) {
            return ParseError{"--top-algorithm 只能是 heap、partial、nth 或 sort，实际为: " +
                              std::string(*algorithm_value)};
        }
        options.top_algorithm = *top_algorithm;
    }
    options.verbose = parsed_values_.contains(std::string_view{"verbose"});
    return options;
}

void CommandLineParser::PrintUsage(std::ostream& output) {
    constexpr int kFlagColumnWidth = 34;
    const std::ios_base::fmtflags original_flags = output.flags();  // std::left 是“粘性”的，用完要恢复

    output << "filesearch —— 本地文件搜索与清理助手\n\n"
           << "用法: filesearch [选项] [目录]\n\n"
           << "选项:\n";
    for (const OptionSpec& option_spec : kOptionSpecs) {
        std::string flag_text = option_spec.short_name != '\0' ? std::string{'-', option_spec.short_name, ','} : "   ";
        flag_text.append(" --").append(option_spec.long_name);
        if (!option_spec.value_hint.empty()) {
            flag_text.append(" ").append(option_spec.value_hint);
        }
        output << "  " << std::left << std::setw(kFlagColumnWidth) << flag_text << option_spec.description << '\n';
    }
    output << "\n示例:\n"
           << R"(  filesearch -d "C:\Downloads" -e .mp4 -s 100MB -t 8)" << '\n'
           << R"(  filesearch D:\Projects -r "^report.*\.docx$" --sort time)" << '\n'
           << R"(  filesearch C:\Users\me -m stats,top -k 20)" << '\n'
           << R"(  filesearch D:\Photos -m dup -e .jpg,.png -s 100K)" << '\n';
    output.flags(original_flags);
}

}  // namespace filesearch::cli
