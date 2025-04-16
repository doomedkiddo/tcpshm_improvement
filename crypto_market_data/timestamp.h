#pragma once
#include <cstdint>
#include <chrono>
#include <string>

// 获取当前纳秒级时间戳
inline int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
        high_resolution_clock::now().time_since_epoch()
    ).count();
}

// 获取当前毫秒级时间戳
inline int64_t now_ms() {
    return now_ns() / 1000000;
}

// 获取当前微秒级时间戳 
inline int64_t now_us() {
    return now_ns() / 1000;
}

// 格式化时间戳为可读字符串 (ISO 8601格式)
inline std::string format_timestamp(int64_t timestamp_ns) {
    using namespace std::chrono;
    auto tp = system_clock::time_point(nanoseconds(timestamp_ns));
    auto time_t = system_clock::to_time_t(tp);
    
    struct tm result;
    localtime_r(&time_t, &result);
    
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &result);
    
    // 添加毫秒部分
    auto ms = (timestamp_ns % 1000000000) / 1000000;
    char full_buffer[40];
    snprintf(full_buffer, sizeof(full_buffer), "%s.%03ld", buffer, ms);
    
    return std::string(full_buffer);
} 