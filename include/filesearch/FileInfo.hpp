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

}  // namespace filesearch
