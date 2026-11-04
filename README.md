# filesearch —— CLI 本地文件搜索与清理助手

一个用现代 C++（C++20）标准库写成的命令行工具：多线程扫描目录树，按后缀/大小/文件名过滤，统计空间占用，找出最大的文件和重复文件，并可把扫描结果缓存为 CSV 索引。程序只读不删，所有清理决定由你自己做。

```
[用户输入]
   │
   ▼
[1. 参数解析] CommandLineParser —— 状态机 + unordered_map
   │
   ▼
[5. 缓存读取] CacheStore::Load（可选）
   │
   ▼
[2. 扫描引擎] ScanEngine —— std::thread 工作队列 / std::async ──> [进度 (atomic + jthread)]
   │
   ▼ std::vector<FileInfo>
[5. 缓存写入] CacheStore::Save（可选）
   │
   ▼
[3. 过滤与分析] FileFilter / FileAnalyzer / DuplicateDetector
   ├─> 搜索模式  search  views::filter 筛选 + ranges::sort 排序
   ├─> 统计模式  stats   unordered_map 统计后缀占用
   ├─> Top K     top     priority_queue 小顶堆等五种实现
   ├─> 重复检测  dup     大小分组 -> 快速哈希 -> 全文哈希
   └─> 基准测试  bench   chrono 对比各实现耗时
   │
   ▼
[4. 输出渲染] ConsoleRenderer —— iomanip 表格
```

## 构建

需要支持 C++20 的编译器（已验证 GCC 16 / MSYS2 MinGW-w64 与 MSVC 14.51 / VS 2026）和 CMake ≥ 3.20。

```bash
# MinGW（MSYS2）
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release
cmake --build build

# MSVC：在 “x64 Native Tools Command Prompt” 中执行
cmake -S . -B out\msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out\msvc
```

也可以直接用 Visual Studio / CLion 打开目录（CMake 项目）。MinGW 构建默认静态链接运行库，避免运行时加载到 PATH 里其他版本的 `libstdc++-6.dll`（例如 Git 自带的那份）。

## 用法

```text
filesearch [选项] [目录]

  -m, --mode <search|stats|top|dup|bench>  运行模式，可用逗号组合（默认 search）
  -d, --directory <path>                   根目录（默认当前目录，也可作为位置参数）
  -e, --extension <list>                   后缀过滤，逗号分隔，可重复：-e .mp4,.mkv -e avi
  -s, --min-size <size> / -S, --max-size   大小过滤：100MB、1.5G、512K、4096
  -n, --name <text>                        文件名包含文本（不区分大小写）
  -r, --regex <pattern>                    文件名正则（ECMAScript，不区分大小写，支持 [一-龥]）
      --sort <size|path|time>              搜索结果排序
  -l, --limit <n>                          每张表最多显示多少行/组（0 = 全部）
  -k, --top <n>                            top 模式的 K（默认 10）
      --top-algorithm <heap|partial|nth|ranges|sort>
  -t, --threads <n>                        扫描线程数（默认 CPU 逻辑核心数）
      --strategy <queue|async>             并发策略
  -c, --cache / --cache-file <file>        启用缓存索引 / 指定缓存文件
      --refresh                            忽略旧缓存，完整扫描并重写
      --timing                             输出各模块耗时
  -v, --verbose                            显示全部扫描错误
```

```bash
filesearch -d "C:\Downloads" -e .mp4 -s 100MB -t 8      # 找下载目录里超过 100MB 的 mp4
filesearch C:\Users\me -m stats,top -k 20               # 后缀占用 + 最大的 20 个文件
filesearch D:\Photos -m dup -e .jpg,.png -s 100K        # 照片重复检测
filesearch D:\Data -c --timing                          # 第二次运行起复用缓存，并显示耗时
filesearch C:\msys64 -m bench -k 10                     # 对比 copy_if / views::filter 与各 Top K 算法
```

选项写法：`-t 8`、`-t8`、`--threads 8`、`--threads=8` 都可以；`--` 之后的参数一律视为位置参数。退出码：0 成功，1 运行错误，2 参数错误。

## 迭代阶段

每个阶段都是一个 git 提交并打了标签，可以 `git checkout stage-1` 查看当时的完整代码，或 `git diff stage-2 stage-3` 看某一阶段改了什么。

| 标签 | 内容 | 练习重点 |
|---|---|---|
| `stage-1` | 单线程 `recursive_directory_iterator` 扫描，`std::sort` 排序，`std::cout` 打印 | `<filesystem>` `<vector>` `<algorithm>` |
| `stage-2` | 工作队列多线程扫描、`std::async` 备选策略、原子计数器进度 | `<thread>` `<mutex>` `<condition_variable>` `<atomic>` `<future>` |
| `stage-3` | 状态机命令行解析、后缀/大小/子串/正则过滤 | `<regex>` `<string_view>` `<charconv>` `<optional>` `<variant>` |
| `stage-4` | 后缀统计、Top K、重复文件检测、iomanip 表格 | `<unordered_map>` `<queue>` `<iomanip>` `<sstream>` `<format>` |
| `stage-5` | CSV 缓存索引、ranges 改写、模块计时与基准测试 | `<fstream>` `<chrono>` `<ranges>` |

## 模块与标准库 API 映射

| 模块 | 文件 | 用到的标准库 API |
|---|---|---|
| 1 参数解析 | `CommandLineParser` | 手写状态机（`ExpectToken` / `ExpectValue` / `PositionalOnly`）；`std::unordered_map` 存原始参数，C++20 透明哈希（`is_transparent` + `std::equal_to<>`）让 `find` / `contains` 直接用 `string_view` 查找；`try_emplace`；`std::string_view::starts_with` / `substr`；`constexpr std::array` 选项表 + `std::find_if`；`std::from_chars`（整数与浮点）；`std::optional`；`std::variant` + `std::holds_alternative` / `std::get_if`；`std::sort` + `std::unique` 去重 |
| 2 扫描引擎 | `ScanEngine` | `directory_iterator` / `recursive_directory_iterator`（`disable_recursion_pending`）、`skip_permission_denied`、`symlink_status`、带 `std::error_code` 的重载与 `increment`；`std::thread` + `std::deque` 工作队列 + `std::mutex` / `std::condition_variable::wait(谓词)`；`std::atomic::fetch_add(memory_order_relaxed)`；`std::jthread` + `std::stop_token` + `condition_variable_any::wait_for(lock, stop_token, ...)`；`std::async` + `std::future`；`std::exception_ptr` / `std::rethrow_exception`；`std::make_move_iterator` 批量合并 |
| 3 过滤与分析 | `FileFilter` `FileAnalyzer` `DuplicateDetector` | `std::copy_if` 与 `std::views::filter` + `std::ranges::copy` 两种过滤；`std::wregex` + `regex_search`（`icase`、`optimize`）；`std::ranges::sort` / `stable_sort` 加投影；`std::unordered_map` 后缀统计；`std::priority_queue` + `std::reference_wrapper` 小顶堆；`std::partial_sort_copy` / `std::nth_element` / `std::ranges::partial_sort_copy`；`std::span`；`std::views::transform(&FileInfo::size)` + `std::accumulate`；自定义键的 `unordered_map` 与 `operator== = default`；`std::ifstream::read` / `gcount` / `peek`；`thread_local` 缓冲区；`future::wait_for` 轮询进度 |
| 4 输出渲染 | `ConsoleRenderer` | `std::setw` / `std::left` / `std::right` / `std::setfill` / `std::setprecision` / `std::fixed` / `std::hex`；`std::ostringstream` 实现 `FormatBytes`；保存与恢复 `flags()` / `precision()` / `fill()` 的 RAII 守卫；`std::format` + `std::chrono::zoned_time` / `current_zone` / `clock_cast` 显示本地时间；UTF-8 解码计算显示宽度，让中文列对齐 |
| 5 持久化 | `CacheStore` | `std::ofstream` 写 / `std::ifstream` + `std::getline` 逐行读；`std::istringstream` + `std::getline(..., ',')` 切字段（复用流对象：`clear()` + `str()`）；`std::filesystem::rename` / `create_directories` / `temp_directory_path`；`clock_cast` / `time_point_cast` / `duration_cast` 处理时间戳 |
| 计时 | `ExecutionTimer.hpp` | `std::chrono::steady_clock`（`static_assert(is_steady)`）；RAII `ScopedTimer`；`std::invoke`；`std::ranges::find` 加投影 |

代码约定：变量名用完整单词（`callback`、`directory`、`extension`，不写 `cb`、`dir`、`ext`）；凡是回调（`std::function`、传给算法的比较器和谓词、线程入口函数），在调用处用 `//call namespace::class::function` 注明实际调用的函数。

## 设计说明

**两种并发策略。** 默认的 `queue` 策略用固定数量的线程从共享目录队列取活，每扫完一个目录把子目录放回队列，负载自动均衡。`async` 策略按根目录的子文件夹划分任务，某个子文件夹特别大时，其他线程会提前闲下来。在 `C:\msys64`（5.2 万文件）上用 8 线程测得 queue 0.33 s、async 0.79 s。线程先把结果攒在本地，每 4096 条才加锁合并一次，以减少锁竞争。

**错误不中断扫描。** queue 策略对每个目录单独构造 `directory_iterator`，打不开的目录记一条错误后继续。`recursive_directory_iterator` 的 `increment` 一旦失败就变成 end 迭代器，整棵子树剩下的部分都会丢失，所以它只出现在阶段 1 和 async 策略里。

**缓存怎样判断“没变化”。** 思路和 mlocate 的 updatedb 相同：缓存里记下每个目录的最后修改时间。目录里新增、删除、改名条目时，目录自身的修改时间会变；时间没变的目录直接复用上次的文件列表，不再遍历。所有目录都命中时也不再重写缓存文件。目录比对的是修改时间，只改文件内容不会更新所在目录的时间，所以文件大小的原地变化检测不到，这时用 `--refresh` 强制完整扫描。缓存文件先写成 `.tmp` 再改名替换；如果 `--cache-file` 指向的已有文件不是本程序写的（首行签名不对），程序不读取也不覆盖它。

**重复检测。** 先按大小分组，再比较前 16 KB 的哈希，最后才对仍然相同的文件计算全文 FNV-1a 64 位哈希。FNV-1a 不是密码学哈希，同组文件应视为“高度疑似相同”，删除前请自行确认。空文件不参与比较。

## 已知差异与限制

- **MSVC 版本扫描更快**：`C:\msys64` 上热缓存时 MSVC 约 40 ms，MinGW 约 260 ms。MSVC 的 `directory_entry` 直接缓存枚举时拿到的大小和时间；MinGW 的 libstdc++ 对每个文件都要额外调用 stat。
- 在 MSVC 版本、本地 SSD、系统文件缓存已经热的情况下，读取 CSV 缓存可能比直接扫描还慢。缓存适合冷启动、机械硬盘、网络盘，以及 stat 开销大的 MinGW 构建。
- MinGW 的 libstdc++ 取得的文件时间只精确到秒，同一秒内发生的目录变化可能被缓存漏掉；MSVC 精确到 100 ns。
- MinGW 版本会把部分没有访问权限的联接点（如 `C:\ProgramData\桌面`）报告为 `Invalid argument [generic:22]`；MSVC 版本会直接跳过这些目录，不报错。
- 正则匹配的是文件名，不含目录部分；大小写不敏感只对 ASCII 字母生效。
