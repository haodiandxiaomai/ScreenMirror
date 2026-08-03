#include "perf_logger.h"

int64_t NowNs() {
    // Use a process-relative QPC origin.  Computing absolute
    // QPC * 1e9 can overflow on machines with long uptime, which makes
    // the logger appear stuck at 0.0s and can cause all data rows to be
    // rejected by the 10s guard.
    static LARGE_INTEGER freq = []{
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    static LARGE_INTEGER origin = []{
        LARGE_INTEGER o{};
        QueryPerformanceCounter(&o);
        return o;
    }();
    if (freq.QuadPart <= 0) return 0;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    int64_t ticks = now.QuadPart - origin.QuadPart;
    if (ticks < 0) ticks = 0;

    const int64_t seconds = ticks / freq.QuadPart;
    const int64_t remain = ticks % freq.QuadPart;
    const int64_t nsRemainder = static_cast<int64_t>(
        (static_cast<long double>(remain) * 1000000000.0L) /
        static_cast<long double>(freq.QuadPart));
    return seconds * 1000000000LL + nsRemainder;
}

PerfCsvLogger g_perfLog;

double DiffMs(int64_t endNs, int64_t beginNs) {
    return (std::max)(0.0, double(endNs - beginNs) / 1000000.0);
}
