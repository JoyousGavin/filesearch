#pragma once

#include <string>
#include <vector>

namespace filesearch::platform {

// RAII：构造时把 Windows 控制台输出切换为 UTF-8，析构时恢复原代码页。非 Windows 平台什么都不做。
class ConsoleEncodingGuard {
public:
    ConsoleEncodingGuard();
    ~ConsoleEncodingGuard();

    ConsoleEncodingGuard(const ConsoleEncodingGuard&) = delete;
    ConsoleEncodingGuard& operator=(const ConsoleEncodingGuard&) = delete;

private:
    unsigned int original_output_code_page_ = 0;
};

// 以 UTF-8 返回命令行参数。Windows 下 argv 是 ANSI 代码页（如 GBK），这里改用 GetCommandLineW 重新取宽字符参数。
std::vector<std::string> CollectUtf8Arguments(int argument_count, char** argument_values);

}  // namespace filesearch::platform
