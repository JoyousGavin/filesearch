#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace filesearch {

// 一条扫描结果。由扫描引擎产生，被过滤、分析、渲染、缓存模块消费。
struct FileInfo {
    std::filesystem::path path;                         // 完整路径（Windows 下内部为 UTF-16）
    std::string extension;                              // 小写 UTF-8 后缀，含点号，如 ".mp4"；无后缀时为空
    std::uintmax_t size = 0;                            // 字节数
    std::filesystem::file_time_type last_write_time{};  // 最后修改时间
};

// 排序比较器（严格弱序）。用具名静态函数而不是匿名 lambda，便于在调用处标注“调用了哪个函数”。
class FileOrdering {
public:
    // 大小降序；大小相同时按路径升序，保证输出稳定可复现
    static bool BySizeDescending(const FileInfo& left, const FileInfo& right) {
        if (left.size != right.size) {
            return left.size > right.size;
        }
        return left.path.native() < right.path.native();
    }

    static bool ByPathAscending(const FileInfo& left, const FileInfo& right) {
        return left.path.native() < right.path.native();
    }

    // 修改时间降序（最新的在前）
    static bool ByTimeDescending(const FileInfo& left, const FileInfo& right) {
        if (left.last_write_time != right.last_write_time) {
            return left.last_write_time > right.last_write_time;
        }
        return left.path.native() < right.path.native();
    }
};

}  // namespace filesearch
