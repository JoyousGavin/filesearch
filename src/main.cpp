// 入口：只负责编码初始化、命令行解析和顶层异常处理，业务流程在 filesearch::app::Application
#include "filesearch/Application.hpp"
#include "filesearch/CommandLineParser.hpp"
#include "filesearch/Platform.hpp"

#include <exception>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

int main(int argument_count, char** argument_values) {
    const filesearch::platform::ConsoleEncodingGuard console_encoding_guard;
    const std::vector<std::string> arguments =
        filesearch::platform::CollectUtf8Arguments(argument_count, argument_values);

    filesearch::cli::CommandLineParser command_line_parser;
    const filesearch::cli::ParseOutcome parse_outcome = command_line_parser.Parse(arguments);

    if (std::holds_alternative<filesearch::cli::HelpRequest>(parse_outcome)) {
        filesearch::cli::CommandLineParser::PrintUsage(std::cout);
        return filesearch::app::kExitSuccess;
    }
    if (const auto* parse_error = std::get_if<filesearch::cli::ParseError>(&parse_outcome)) {
        std::cerr << "参数错误: " << parse_error->message << "\n使用 --help 查看用法\n";
        return filesearch::app::kExitUsageError;
    }

    try {
        filesearch::app::Application application(std::cout, std::cerr);
        return application.Run(std::get<filesearch::cli::CommandLineOptions>(parse_outcome));
    } catch (const std::exception& exception) {
        std::cerr << "\n运行失败: " << exception.what() << '\n';
        return filesearch::app::kExitRuntimeError;
    }
}
