#pragma once

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

#ifdef __linux__
#include <unistd.h>
#endif

namespace linrtx {

namespace detail {
struct NativeLogState {
    std::mutex mu;
    std::FILE* fp = nullptr;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

inline NativeLogState& logState() {
    static NativeLogState state;
    return state;
}
} // namespace detail

inline void closeNativeLog() {
    auto& st = detail::logState();
    std::lock_guard<std::mutex> lock(st.mu);
    if (st.fp) {
        std::fflush(st.fp);
        std::fclose(st.fp);
        st.fp = nullptr;
    }
}

inline void initNativeLog(const char* logDir) {
    namespace fs = std::filesystem;
    auto& st = detail::logState();
    std::lock_guard<std::mutex> lock(st.mu);

    if (st.fp) {
        std::fflush(st.fp);
        std::fclose(st.fp);
        st.fp = nullptr;
    }

    st.t0 = std::chrono::steady_clock::now();
    std::string dir = (logDir && *logDir) ? logDir : "/tmp";
    std::error_code ec;
    fs::create_directories(dir, ec);

    fs::path cur = fs::path(dir) / "linrtx-native.log";
    fs::path prev = fs::path(dir) / "linrtx-native.previous.log";
    if (fs::exists(cur, ec)) {
        fs::rename(cur, prev, ec);
    }

    st.fp = std::fopen(cur.c_str(), "w");
    if (st.fp) {
        // Unbuffered writes ensure crash breadcrumbs hit disk immediately before any driver fault.
        std::setvbuf(st.fp, nullptr, _IONBF, 0);
        std::fprintf(st.fp, "[0ms] === LinRTX Native Session Start ===\n");
        std::fflush(st.fp);
    }
}

inline void logNative(const char* fmt, ...) {
    auto& st = detail::logState();
    std::lock_guard<std::mutex> lock(st.mu);
    if (!st.fp || !fmt) return;

    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - st.t0).count();
    std::fprintf(st.fp, "[%lldms] ", static_cast<long long>(ms));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(st.fp, fmt, args);
    va_end(args);

    std::fputc('\n', st.fp);
    std::fflush(st.fp);
#ifdef __linux__
    ::fsync(fileno(st.fp));
#endif
}

} // namespace linrtx
