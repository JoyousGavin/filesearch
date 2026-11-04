#pragma once

#include "filesearch/FileInfo.hpp"
#include "filesearch/ScanEngine.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace filesearch::cache {

// 缓存索引：按目录组织的上一次扫描结果，实现 DirectorySnapshotProvider 接口供扫描引擎查询。
// 扫描期间多个线程只读访问，不需要加锁。
class CacheIndex final : public scan::DirectorySnapshotProvider {
public:
    const scan::DirectorySnapshot* FindDirectory(const std::filesystem::path& directory) const override;

    std::filesystem::path root_directory;
    // 键为 path::native()（Windows 下是 std::wstring），值为该目录的快照
    std::unordered_map<std::filesystem::path::string_type, scan::DirectorySnapshot> directories;
    std::size_t file_count = 0;
};

enum class LoadStatus {
    Loaded,
    NotFound,      // 缓存文件不存在（第一次运行）
    RootMismatch,  // 缓存属于另一个根目录
    ForeignFile,   // 文件存在但首行签名不对：不是本程序写的，既不读取也不覆盖
    Invalid,       // 签名正确，但表头或某一行格式不对（例如被手工改坏）
    Skipped,       // 用户指定了 --refresh，没有读取
};

struct LoadResult {
    LoadStatus status = LoadStatus::NotFound;
    std::optional<CacheIndex> index;  // 只有 Loaded 时有值
    std::string detail;               // 失败原因，或 RootMismatch 时缓存所属的根目录
};

// 缓存文件格式（CSV，UTF-8）：
//   #filesearch-cache v1
//   #root,<根目录>
//   type,path,size,extension,timestamp
//   D,<目录>,,,<修改时间>        修改时间为空表示上次没能完整读取，下次必须重新遍历
//   F,<文件>,<字节数>,<后缀>,<修改时间>
// 修改时间统一存为 Unix 纪元以来的纳秒数
class CacheStore {
public:
    explicit CacheStore(std::filesystem::path cache_file);

    // 默认位置：系统临时目录/filesearch/cache_<根目录路径哈希>.csv，每个根目录一份
    static std::filesystem::path DefaultCacheFile(const std::filesystem::path& root_directory);

    const std::filesystem::path& CacheFile() const;

    LoadResult Load(const std::filesystem::path& expected_root) const;

    // 成功返回 std::nullopt，失败返回错误信息。
    // 先写 .tmp 临时文件再改名替换，避免中途失败留下半个缓存；目标已存在但不是本程序的缓存文件时拒绝覆盖。
    std::optional<std::string> Save(const std::filesystem::path& root_directory,
                                    const scan::ScanResult& scan_result) const;

    // 字段含逗号、引号或换行时用引号包起来，并把 " 写成 ""
    static std::string EscapeCsvField(std::string_view field);
    // std::istringstream + std::getline(..., ',') 切分，再把被引号包住的逗号拼回去；引号不配对时返回 std::nullopt
    static std::optional<std::vector<std::string>> SplitCsvLine(const std::string& line);
    // 同上，但复用调用方的流对象：逐行解析大文件时避免每行都构造一次 istringstream（构造开销不小）
    static std::optional<std::vector<std::string>> SplitCsvLine(const std::string& line,
                                                                std::istringstream& reusable_stream);

private:
    static bool HasCacheSignature(const std::filesystem::path& file);

    std::filesystem::path cache_file_;
};

}  // namespace filesearch::cache
