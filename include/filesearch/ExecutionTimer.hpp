#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <numeric>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace filesearch::timing {

// 单调时钟：不受系统时间被修改的影响，适合测量耗时（system_clock 适合表示“现在几点”）
using Clock = std::chrono::steady_clock;
static_assert(Clock::is_steady, "steady_clock 必须是单调时钟");

struct TimingRecord {
    std::string module_name;
    std::chrono::nanoseconds duration{};
    std::size_t call_count = 0;
};

// 同一操作重复运行多次的统计：最快值受系统噪声影响最小，平均值反映整体水平
struct BenchmarkResult {
    std::string operation_name;
    std::chrono::nanoseconds fastest_duration{};
    std::chrono::nanoseconds average_duration{};
    std::size_t result_size = 0;  // 操作结果的元素个数，用来核对各实现结果一致，同时防止编译器把计算优化掉
};

class TimingReport {
public:
    // 同名模块累加（例如多次调用渲染）
    void Add(std::string_view module_name, std::chrono::nanoseconds duration) {
        // ranges::find + 投影：按 TimingRecord::module_name 查找，不需要手写比较 lambda
        const auto found = std::ranges::find(records_, module_name, &TimingRecord::module_name);  //call std::invoke(&TimingRecord::module_name, record) 取出投影后与 module_name 比较
        if (found == records_.end()) {
            records_.push_back(TimingRecord{std::string(module_name), duration, 1});
            return;
        }
        found->duration += duration;
        ++found->call_count;
    }

    std::span<const TimingRecord> Records() const {
        return records_;
    }

    std::chrono::nanoseconds Total() const {
        return std::accumulate(records_.begin(), records_.end(), std::chrono::nanoseconds{0},
                               [](std::chrono::nanoseconds running_total, const TimingRecord& record) {
                                   return running_total + record.duration;
                               });  //call 上面的 lambda（std::accumulate 逐条累加）
    }

private:
    std::vector<TimingRecord> records_;
};

// RAII 计时器：构造时记录起点，析构时把耗时写入 TimingReport
class ScopedTimer {
public:
    ScopedTimer(TimingReport& report, std::string_view module_name)
        : report_(report), module_name_(module_name), start_time_(Clock::now()) {}

    ~ScopedTimer() {
        try {
            report_.Add(module_name_, Elapsed());
        } catch (...) {
            // 析构函数不能抛异常；计时信息丢了也不影响业务结果
        }
    }

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    std::chrono::nanoseconds Elapsed() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_time_);
    }

private:
    TimingReport& report_;
    std::string module_name_;
    Clock::time_point start_time_;
};

// 测量一次调用的耗时
template <typename Callable>
std::chrono::nanoseconds MeasureCall(Callable&& callable) {
    const Clock::time_point start_time = Clock::now();
    std::invoke(std::forward<Callable>(callable));  //call 调用方传入的可调用对象（具体函数见各调用处的注释）
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_time);
}

}  // namespace filesearch::timing
