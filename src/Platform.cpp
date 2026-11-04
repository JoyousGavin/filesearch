#include "filesearch/Platform.hpp"

#include <memory>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace filesearch::platform {

#ifdef _WIN32
namespace {

std::string WideToUtf8(std::wstring_view wide_text) {
    if (wide_text.empty()) {
        return {};
    }
    const int wide_length = static_cast<int>(wide_text.size());
    const int required_bytes =
        ::WideCharToMultiByte(CP_UTF8, 0, wide_text.data(), wide_length, nullptr, 0, nullptr, nullptr);
    std::string utf8_text(static_cast<std::size_t>(required_bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide_text.data(), wide_length, utf8_text.data(), required_bytes, nullptr,
                          nullptr);
    return utf8_text;
}

// CommandLineToArgvW 返回的内存必须用 LocalFree 释放，交给 unique_ptr 管理
struct LocalFreeDeleter {
    void operator()(wchar_t** wide_arguments) const noexcept {
        ::LocalFree(wide_arguments);
    }
};

}  // namespace
#endif

ConsoleEncodingGuard::ConsoleEncodingGuard() {
#ifdef _WIN32
    original_output_code_page_ = ::GetConsoleOutputCP();
    ::SetConsoleOutputCP(CP_UTF8);
#endif
}

ConsoleEncodingGuard::~ConsoleEncodingGuard() {
#ifdef _WIN32
    if (original_output_code_page_ != 0) {
        ::SetConsoleOutputCP(original_output_code_page_);
    }
#endif
}

std::vector<std::string> CollectUtf8Arguments(int argument_count, char** argument_values) {
#ifdef _WIN32
    int wide_argument_count = 0;
    const std::unique_ptr<wchar_t*, LocalFreeDeleter> wide_arguments(
        ::CommandLineToArgvW(::GetCommandLineW(), &wide_argument_count));
    if (wide_arguments) {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(wide_argument_count));
        for (int index = 0; index < wide_argument_count; ++index) {
            arguments.push_back(WideToUtf8(wide_arguments.get()[index]));
        }
        return arguments;
    }
#endif
    return std::vector<std::string>(argument_values, argument_values + argument_count);
}

}  // namespace filesearch::platform
