#include "filesearch/CacheStore.hpp"

#include "filesearch/Platform.hpp"
#include "filesearch/TextUtilities.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <ios>
#include <sstream>
#include <system_error>
#include <utility>

namespace filesearch::cache {

namespace {

constexpr std::string_view kSignatureLine = "#filesearch-cache v1";
constexpr std::string_view kRootPrefix = "#root";
constexpr std::string_view kColumnHeader = "type,path,size,extension,timestamp";
constexpr std::string_view kDirectoryType = "D";
constexpr std::string_view kFileType = "F";
constexpr std::size_t kColumnCount = 5;

// 时间戳存成 Unix 纪元以来的纳秒数：与编译器无关（MSVC 的 file_clock 纪元是 1601 年，libstdc++ 又不同），
// NTFS 的 100ns 精度也能无损往返，修改时间比较才可靠
using SystemDuration = std::chrono::system_clock::duration;

std::int64_t ToUnixNanoseconds(std::filesystem::file_time_type file_time) {
    const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(file_time);
    // int64 纳秒只能表示约 1677~2262 年；超出范围（例如 1601 年的“零时间戳”）时截到边界，避免有符号溢出。
    // 截断后的目录时间与实际不等，只会导致该目录下次重新遍历，不影响正确性
    constexpr SystemDuration kEarliest =
        std::chrono::duration_cast<SystemDuration>(std::chrono::nanoseconds::min() / 2);
    constexpr SystemDuration kLatest = std::chrono::duration_cast<SystemDuration>(std::chrono::nanoseconds::max() / 2);
    const SystemDuration since_epoch = std::clamp(system_time.time_since_epoch(), kEarliest, kLatest);
    return std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
}

std::filesystem::file_time_type FromUnixNanoseconds(std::int64_t unix_nanoseconds) {
    // 先换成 system_clock 自己的精度（MSVC 为 100ns）再 clock_cast。
    // 不能用 sys_time<nanoseconds> 做中间值：MSVC 的 file_clock 从 1601 年起算，换算成纳秒会超出 int64 而溢出
    const SystemDuration since_epoch =
        std::chrono::duration_cast<SystemDuration>(std::chrono::nanoseconds{unix_nanoseconds});
    const std::chrono::sys_time<SystemDuration> system_time{since_epoch};
    const auto file_time = std::chrono::clock_cast<std::filesystem::file_time_type::clock>(system_time);
    return std::chrono::time_point_cast<std::filesystem::file_time_type::duration>(file_time);
}

template <typename Integer>
std::optional<Integer> ParseInteger(std::string_view text) {
    Integer value{};
    const char* const text_end = text.data() + text.size();
    const auto [parse_end, parse_error] = std::from_chars(text.data(), text_end, value);
    if (text.empty() || parse_error != std::errc{} || parse_end != text_end) {
        return std::nullopt;
    }
    return value;
}

// 以二进制模式读取时，用记事本等工具编辑过的文件行尾会多一个 '\r'
std::string_view StripCarriageReturn(std::string_view line) {
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    return line;
}

std::string UnquoteCsvField(std::string_view field) {
    if (field.size() < 2 || field.front() != '"' || field.back() != '"') {
        return std::string(field);
    }
    std::string unquoted;
    unquoted.reserve(field.size() - 2);
    for (std::size_t index = 1; index + 1 < field.size(); ++index) {
        unquoted.push_back(field[index]);
        if (field[index] == '"' && field[index + 1] == '"') {
            ++index;  // "" 还原为一个 "
        }
    }
    return unquoted;
}

LoadResult MakeFailure(LoadStatus status, std::string detail) {
    LoadResult result;
    result.status = status;
    result.detail = std::move(detail);
    return result;
}

}  // namespace

const scan::DirectorySnapshot* CacheIndex::FindDirectory(const std::filesystem::path& directory) const {
    const auto found = directories.find(directory.native());
    return found == directories.end() ? nullptr : &found->second;
}

CacheStore::CacheStore(std::filesystem::path cache_file) : cache_file_(std::move(cache_file)) {}

const std::filesystem::path& CacheStore::CacheFile() const {
    return cache_file_;
}

std::filesystem::path CacheStore::DefaultCacheFile(const std::filesystem::path& root_directory) {
    std::ostringstream file_name;
    file_name << "cache_" << std::hex << std::setw(16) << std::setfill('0')
              << std::hash<std::string>{}(text::PathToUtf8(root_directory)) << ".csv";
    std::error_code temporary_directory_error;
    std::filesystem::path base_directory = std::filesystem::temp_directory_path(temporary_directory_error);
    if (temporary_directory_error) {
        base_directory = std::filesystem::current_path();
    }
    return base_directory / "filesearch" / file_name.str();
}

std::string CacheStore::EscapeCsvField(std::string_view field) {
    if (field.find_first_of(",\"\r\n") == std::string_view::npos) {
        return std::string(field);
    }
    std::string escaped;
    escaped.reserve(field.size() + 2);
    escaped.push_back('"');
    for (const char character : field) {
        if (character == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
}

std::optional<std::vector<std::string>> CacheStore::SplitCsvLine(const std::string& line) {
    std::istringstream line_stream;
    return SplitCsvLine(line, line_stream);
}

std::optional<std::vector<std::string>> CacheStore::SplitCsvLine(const std::string& line,
                                                                 std::istringstream& line_stream) {
    line_stream.clear();    // 清掉上一行读到末尾留下的 eofbit
    line_stream.str(line);  // 换上新的一行内容
    std::vector<std::string> fields;
    std::string segment;
    std::string pending_field;
    bool inside_quotes = false;
    while (std::getline(line_stream, segment, ',')) {
        if (inside_quotes) {
            pending_field.push_back(',');  // 被 getline 吃掉的逗号其实属于引号内的字段内容
            pending_field += segment;
        } else {
            pending_field = segment;
        }
        // 引号个数为奇数说明引号还没闭合（转义的 "" 成对出现，不影响奇偶）
        inside_quotes = std::ranges::count(pending_field, '"') % 2 == 1;
        if (!inside_quotes) {
            fields.push_back(UnquoteCsvField(pending_field));
        }
    }
    if (inside_quotes) {
        return std::nullopt;
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();  // getline 不会为行尾的逗号再产生一个空字段
    }
    return fields;
}

bool CacheStore::HasCacheSignature(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::in | std::ios::binary);
    std::string first_line;
    return input.is_open() && std::getline(input, first_line) && StripCarriageReturn(first_line) == kSignatureLine;
}

LoadResult CacheStore::Load(const std::filesystem::path& expected_root) const {
    std::ifstream input(cache_file_, std::ios::in | std::ios::binary);
    if (!input.is_open()) {
        return MakeFailure(LoadStatus::NotFound, "");
    }

    std::string line;
    if (!std::getline(input, line) || StripCarriageReturn(line) != kSignatureLine) {
        return MakeFailure(LoadStatus::ForeignFile, "首行签名不符");
    }
    if (!std::getline(input, line)) {
        return MakeFailure(LoadStatus::Invalid, "缺少根目录行");
    }
    const std::optional<std::vector<std::string>> root_fields = SplitCsvLine(std::string(StripCarriageReturn(line)));
    if (!root_fields || root_fields->size() != 2 || (*root_fields)[0] != kRootPrefix) {
        return MakeFailure(LoadStatus::Invalid, "根目录行格式错误");
    }
    const std::filesystem::path cached_root = text::PathFromUtf8((*root_fields)[1]);
    if (cached_root != expected_root) {
        return MakeFailure(LoadStatus::RootMismatch, (*root_fields)[1]);
    }
    if (!std::getline(input, line) || StripCarriageReturn(line) != kColumnHeader) {
        return MakeFailure(LoadStatus::Invalid, "缺少表头");
    }

    // 第一遍：逐行解析，目录直接建索引，文件先暂存
    CacheIndex index;
    index.root_directory = cached_root;
    std::vector<std::filesystem::path> directory_paths;
    std::vector<FileInfo> files;
    std::istringstream line_stream;
    std::size_t line_number = 3;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::optional<std::vector<std::string>> fields = SplitCsvLine(line, line_stream);
        if (!fields || fields->size() != kColumnCount) {
            return MakeFailure(LoadStatus::Invalid, std::format("第 {} 行字段数不对", line_number));
        }
        const std::string& record_type = (*fields)[0];
        if (record_type == kDirectoryType) {
            std::filesystem::path directory_path = text::PathFromUtf8((*fields)[1]);
            scan::DirectorySnapshot& snapshot = index.directories[directory_path.native()];
            if (const std::optional<std::int64_t> timestamp = ParseInteger<std::int64_t>((*fields)[4])) {
                snapshot.last_write_time = FromUnixNanoseconds(*timestamp);
            }
            directory_paths.push_back(std::move(directory_path));
        } else if (record_type == kFileType) {
            const std::optional<std::uintmax_t> size = ParseInteger<std::uintmax_t>((*fields)[2]);
            const std::optional<std::int64_t> timestamp = ParseInteger<std::int64_t>((*fields)[4]);
            if (!size || !timestamp) {
                return MakeFailure(LoadStatus::Invalid, std::format("第 {} 行数值格式错误", line_number));
            }
            FileInfo file;
            file.path = text::PathFromUtf8((*fields)[1]);
            file.extension = (*fields)[3];
            file.size = *size;
            file.last_write_time = FromUnixNanoseconds(*timestamp);
            files.push_back(std::move(file));
        } else {
            return MakeFailure(LoadStatus::Invalid, std::format("第 {} 行记录类型未知", line_number));
        }
    }
    if (input.bad()) {
        return MakeFailure(LoadStatus::Invalid, "读取过程中发生 I/O 错误");
    }

    // 第二遍：按 parent_path 把子目录和文件挂到所属目录的快照下
    for (const std::filesystem::path& directory_path : directory_paths) {
        if (directory_path == cached_root) {
            continue;
        }
        const auto parent = index.directories.find(directory_path.parent_path().native());
        if (parent != index.directories.end()) {
            parent->second.subdirectories.push_back(directory_path);
        }
    }
    for (FileInfo& file : files) {
        const auto parent = index.directories.find(file.path.parent_path().native());
        if (parent != index.directories.end()) {
            parent->second.files.push_back(std::move(file));
            ++index.file_count;
        }
    }

    LoadResult result;
    result.status = LoadStatus::Loaded;
    result.index = std::move(index);
    return result;
}

std::optional<std::string> CacheStore::Save(const std::filesystem::path& root_directory,
                                            const scan::ScanResult& scan_result) const {
    std::error_code filesystem_error;
    // 只覆盖本程序自己写的缓存：防止 --cache-file 误指向用户的其他文件
    if (std::filesystem::exists(cache_file_, filesystem_error) && !HasCacheSignature(cache_file_)) {
        return "目标文件已存在且不是 filesearch 缓存，拒绝覆盖: " + text::PathToUtf8(cache_file_);
    }
    if (cache_file_.has_parent_path()) {
        std::filesystem::create_directories(cache_file_.parent_path(), filesystem_error);
        if (filesystem_error) {
            return "无法创建缓存目录: " + platform::DescribeError(filesystem_error);
        }
    }

    std::filesystem::path temporary_file = cache_file_;
    temporary_file += ".tmp";
    {
        std::ofstream output(temporary_file, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            return "无法写入临时文件: " + text::PathToUtf8(temporary_file);
        }
        output << kSignatureLine << '\n'
               << kRootPrefix << ',' << EscapeCsvField(text::PathToUtf8(root_directory)) << '\n'
               << kColumnHeader << '\n';
        for (const scan::DirectoryRecord& directory : scan_result.directories) {
            output << kDirectoryType << ',' << EscapeCsvField(text::PathToUtf8(directory.path)) << ",,,";
            if (directory.last_write_time) {
                output << ToUnixNanoseconds(*directory.last_write_time);
            }
            output << '\n';
        }
        for (const FileInfo& file : scan_result.files) {
            output << kFileType << ',' << EscapeCsvField(text::PathToUtf8(file.path)) << ',' << file.size << ','
                   << EscapeCsvField(file.extension) << ',' << ToUnixNanoseconds(file.last_write_time) << '\n';
        }
        output.flush();
        if (!output) {
            output.close();
            std::filesystem::remove(temporary_file, filesystem_error);
            return "写入缓存失败（磁盘空间不足？）: " + text::PathToUtf8(temporary_file);
        }
    }  // ofstream 在这里析构并关闭文件，之后才能改名

    std::filesystem::rename(temporary_file, cache_file_, filesystem_error);
    if (filesystem_error) {
        // 个别运行库在目标已存在时 rename 会失败：删掉旧缓存（上面已确认是本程序的文件）再试一次
        std::error_code remove_error;
        std::filesystem::remove(cache_file_, remove_error);
        std::filesystem::rename(temporary_file, cache_file_, filesystem_error);
        if (filesystem_error) {
            std::filesystem::remove(temporary_file, remove_error);
            return "无法替换缓存文件: " + platform::DescribeError(filesystem_error);
        }
    }
    return std::nullopt;
}

}  // namespace filesearch::cache
