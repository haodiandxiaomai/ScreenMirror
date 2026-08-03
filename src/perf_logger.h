#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

int64_t NowNs();
double DiffMs(int64_t endNs, int64_t beginNs);

class PerfCsvLogger {
public:
    static constexpr int64_t kMaxCaptureNs = 10LL * 1000000000LL;

    bool isRecording() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return recording_;
    }

    double elapsedMs() const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!recording_ || startNs_ <= 0) return 0.0;
        return (std::max)(0.0, double(NowNs() - startNs_) / 1000000.0);
    }

    size_t lastSavedDataRows() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return lastSavedDataRows_;
    }

    std::wstring toggle(bool& started, bool& saved) {
        started = false;
        saved = false;
        std::vector<std::string> rowsToWrite;
        std::wstring path;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!recording_) {
                startLocked();
                started = true;
                return std::wstring();
            }
            stopLocked(path, rowsToWrite);
            saved = true;
        }
        writeRows(path, rowsToWrite);
        return path;
    }

    bool stopIfExpired(std::wstring& pathOut) {
        std::vector<std::string> rowsToWrite;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!recording_ || startNs_ <= 0) return false;
            int64_t relNs = NowNs() - startNs_;
            if (relNs < 0) relNs = 0;
            if (relNs < kMaxCaptureNs) return false;
            stopLocked(pathOut, rowsToWrite);
        }
        writeRows(pathOut, rowsToWrite);
        return true;
    }

    bool stop(std::wstring& pathOut) {
        std::vector<std::string> rowsToWrite;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!recording_) return false;
            stopLocked(pathOut, rowsToWrite);
        }
        writeRows(pathOut, rowsToWrite);
        return true;
    }

    void recordRow(const char* eventName, const char* payloadCsv) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!recording_ || startNs_ <= 0) return;
        const int64_t nowNs = NowNs();
        int64_t relNs = nowNs - startNs_;
        if (relNs < 0) relNs = 0;
        if (relNs > kMaxCaptureNs) return;

        char prefix[128]{};
        const double relMs = double(relNs) / 1000000.0;
        std::snprintf(prefix, sizeof(prefix), "%.3f,%s,", relMs, eventName ? eventName : "event");
        std::string row(prefix);
        row += payloadCsv ? payloadCsv : "";
        row += "\n";
        rows_.push_back(std::move(row));
    }

private:
    mutable std::mutex mutex_;
    bool recording_ = false;
    int64_t startNs_ = 0;
    std::vector<std::string> rows_;
    std::wstring lastPath_;
    size_t lastSavedDataRows_ = 0;

    void startLocked() {
        recording_ = true;
        startNs_ = NowNs();
        rows_.clear();
        rows_.reserve(50000);
        lastSavedDataRows_ = 0;
        rows_.push_back(
            "rel_ms,event,seq,generation,frame_produced_ns,width,height,part_index,part_count,"
            "full_width,full_height,part_left,part_top,part_width,part_height,encoded_width,encoded_height,"
            "jpeg_kb,android_capture_ms,android_encode_ms,android_queue_ms,pc_socket_ms,pc_recv_read_ms,"
            "pc_decode_wall_ms,pc_decode_cpu_sum_ms,pc_decode_max_part_ms,pc_decode_tail_ms,pc_decode_overlap_saved_ms,"
            "pc_upload_ms,pc_draw_ms,pc_present_ms,present_interval_ms,skipped_frames,recv_mbps,display_fps,"
            "cpu_id,cpu_freq_khz,part_encode_ms,upload_mode,upload_row_pitch,note\n");
        lastPath_.clear();
    }

    std::wstring makePathLocked() const {
        wchar_t cwd[MAX_PATH]{};
        DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
        std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(cwd) : std::wstring(L".");
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[128]{};
        swprintf_s(name, 128, L"huilang_perf_%04u%02u%02u_%02u%02u%02u.csv",
            static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth), static_cast<unsigned>(st.wDay),
            static_cast<unsigned>(st.wHour), static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
        if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L"\\";
        dir += name;
        return dir;
    }

    void stopLocked(std::wstring& path, std::vector<std::string>& rowsToWrite) {
        recording_ = false;
        path = makePathLocked();
        lastPath_ = path;
        lastSavedDataRows_ = rows_.empty() ? 0 : rows_.size() - 1;
        rowsToWrite.swap(rows_);
        startNs_ = 0;
    }

    static void writeRows(const std::wstring& path, const std::vector<std::string>& rows) {
        if (path.empty() || rows.empty()) return;
        FILE* fp = nullptr;
        if (_wfopen_s(&fp, path.c_str(), L"wb") != 0 || !fp) return;
        for (const auto& row : rows) {
            if (!row.empty()) {
                std::fwrite(row.data(), 1, row.size(), fp);
            }
        }
        std::fclose(fp);
    }
};

extern PerfCsvLogger g_perfLog;
