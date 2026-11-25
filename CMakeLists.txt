cmake_minimum_required(VERSION 3.23...3.31)

project(FileSearch
    VERSION 0.1.0
    DESCRIPTION "CLI local file search and cleanup assistant"
    LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(filesearch
    src/main.cpp
    src/Application.cpp
    src/ConsoleRenderer.cpp
    src/DuplicateDetector.cpp
    src/FileAnalyzer.cpp
    src/CacheStore.cpp
    src/CommandLineParser.cpp
    src/FileFilter.cpp
    src/Platform.cpp
    src/ScanEngine.cpp
    src/TextUtilities.cpp
)

target_include_directories(filesearch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(filesearch PRIVATE cxx_std_20)
set_target_properties(filesearch PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(filesearch PRIVATE Threads::Threads)

if(MSVC)
    # /utf-8: 源码与执行字符集都按 UTF-8 处理（源码里有中文注释和中文提示）
    target_compile_options(filesearch PRIVATE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
else()
    target_compile_options(filesearch PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

if(WIN32)
    target_compile_definitions(filesearch PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    # CommandLineToArgvW 位于 shell32
    target_link_libraries(filesearch PRIVATE shell32)
endif()

if(MINGW)
    # 静态链接运行库：避免运行时加载到 PATH 中其他 MinGW（如 Git 自带）的不兼容 libstdc++-6.dll
    target_link_options(filesearch PRIVATE -static)
endif()
