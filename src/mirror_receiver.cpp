#include "mirror_receiver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>
#include <turbojpeg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hl_common.h"
#include "mirror_net.h"
#include "perf_logger.h"

// ---------- BufferedSocketReader 类（完全不变） ----------
class BufferedSocketReader {
public:
    explicit BufferedSocketReader(SOCKET s, size_t capacity = 1024 * 1024)
        : socket_(s), buffer_(capacity) {}

    void reset(SOCKET s) {
        socket_ = s;
        begin_ = 0;
        end_ = 0;
    }

    bool readAll(uint8_t* dst, int size) {
        if (size < 0) return false;
        int copied = 0;
        while (copied < size) {
            const size_t available = end_ - begin_;
            if (available > 0) {
                const size_t take = (std::min)(available, static_cast<size_t>(size - copied));
                std::memcpy(dst + copied, buffer_.data() + begin_, take);
                begin_ += take;
                copied += static_cast<int>(take);
                if (begin_ == end_) {
                    begin_ = 0;
                    end_ = 0;
                }
                continue;
            }

            const int remaining = size - copied;
            if (remaining >= static_cast<int>(buffer_.size() / 2)) {
                int n = recv(socket_, reinterpret_cast<char*>(dst + copied), remaining, 0);
                if (n <= 0) return false;
                copied += n;
                continue;
            }

            int n = recv(socket_, reinterpret_cast<char*>(buffer_.data()), static_cast<int>(buffer_.size()), 0);
            if (n <= 0) return false;
            begin_ = 0;
            end_ = static_cast<size_t>(n);
        }
        return true;
    }

private:
    SOCKET socket_ = INVALID_SOCKET;
    std::vector<uint8_t> buffer_;
    size_t begin_ = 0;
    size_t end_ = 0;
};

// ---------- MirrorReceiver::Impl 实现 ----------
class MirrorReceiver::Impl {
public:
    explicit Impl(SharedState& state, int port = PORT)
        : state_(state), port_(port) {}
    ~Impl() { stop(); }

    void start() {
        stopped_.store(false);
        netThread_ = std::thread([this] { runNet(); });
        decodeThread_ = std::thread([this] { runDecode(); });
        splitPublishThread_ = std::thread([this] { runSplitPublish(); });
    }

    void stop() {
        if (stopped_.exchange(true)) return;
        closesocket(sock_);
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            queue_.clear();
        }
        queueCv_.notify_all();
        {
            std::lock_guard<std::mutex> lk(splitPublishMutex_);
            splitPublishQueue_.clear();
        }
        splitPublishCv_.notify_all();
        if (netThread_.joinable()) netThread_.join();
        if (decodeThread_.joinable()) decodeThread_.join();
        if (splitPublishThread_.joinable()) splitPublishThread_.join();
    }

private:
    // ---------- 内部数据结构 ----------
    struct CompressedFramePart {
        FrameHeader header{};
        std::vector<uint8_t> jpeg;
        int64_t recvBeginNs = 0;
        int64_t recvDoneNs = 0;
        int partIndex = 0;
        int partCount = 1;
        int fullWidth = 0;
        int fullHeight = 0;
        int partLeft = 0;
        int partTop = 0;
        int partWidth = 0;
        int partHeight = 0;
        int partEncodeUs = 0;
        int partCpuId = -1;
        int partCpuFreqKhz = 0;
        int partSharePermille = 0;
        int androidWorkerId = -1;
        int androidSendOrder = -1;
        int androidDispatchDelayUs = 0;
        int androidWaitBeforeWriteUs = 0;
        int androidWriteBeforeUs = 0;
        int androidPreviousWriteUs = 0;
        int androidWriterCpuId = -1;
        int androidSenderState = 0;
        int androidFlags = 0;
        double pcArrivalGapMs = 0.0;
        double pcFrameRecvSpanMs = 0.0;
        bool skipped = false;
    };

    struct PendingCompressedFrame {
        FrameHeader header{};
        std::vector<uint8_t> jpeg;
        int64_t recvBeginNs = 0;
        int64_t recvDoneNs = 0;
        uint64_t seq = 0;
        uint64_t streamEpoch = 0;
        bool split = false;
        int fullWidth = 0;
        int fullHeight = 0;
        std::vector<CompressedFramePart> parts;
    };

    struct StreamingSplitDecodeState {
        std::mutex mutex;
        std::condition_variable cv;
        uint64_t streamEpoch = 0;
        uint64_t seq = 0;
        FrameHeader header{};
        int fullWidth = 0;
        int fullHeight = 0;
        int fullPitch = 0;
        int expectedParts = 0;
        int receivedParts = 0;
        int decodedParts = 0;
        bool failed = false;
        int64_t recvBeginNs = 0;
        int64_t recvDoneNs = 0;
        int64_t lastPartRecvDoneNs = 0;
        uint64_t totalBytes = 0;
        uint64_t part0Bytes = 0;
        uint64_t part1Bytes = 0;
        int availableEncodeCpuCount = 0;
        DecodedFrame frame;
        std::vector<CompressedFramePart> parts;
        std::vector<uint8_t> received;
        std::vector<int64_t> dStart;
        std::vector<int64_t> dEnd;
        std::vector<int> ok;
        int64_t decodeFirstStartNs = 0;
        int64_t decodeLastEndNs = 0;
        int64_t decodeCpuSumNs = 0;
        int64_t decodeMaxPartNs = 0;
    };

    // ---------- SplitDecodePool ----------
    class SplitDecodePool {
    public:
        SplitDecodePool() = default;
        ~SplitDecodePool() { stop(); }

        bool decode(
                const std::vector<CompressedFramePart>& parts,
                uint8_t* bgraBase,
                int fullPitch,
                std::vector<int64_t>& dStart,
                std::vector<int64_t>& dEnd,
                std::vector<int>& ok) {
            const int partCount = static_cast<int>(parts.size());
            if (partCount <= 0 || partCount > kMaxWorkers || bgraBase == nullptr || fullPitch <= 0) return false;
            ensureStarted();

            auto batch = std::make_shared<SyncBatch>();
            batch->expected = partCount;
            batch->dStart.assign(static_cast<size_t>(partCount), 0);
            batch->dEnd.assign(static_cast<size_t>(partCount), 0);
            batch->ok.assign(static_cast<size_t>(partCount), 0);

            {
                std::lock_guard<std::mutex> lk(mutex_);
                for (int i = 0; i < partCount; ++i) {
                    WorkItem wi;
                    wi.part = &parts[static_cast<size_t>(i)];
                    wi.dst = bgraBase + static_cast<size_t>(wi.part->partTop) * static_cast<size_t>(fullPitch) + static_cast<size_t>(wi.part->partLeft) * 4u;
                    wi.pitch = fullPitch;
                    wi.syncBatch = batch;
                    wi.slotIndex = i;
                    queue_.push_back(std::move(wi));
                }
            }
            cv_.notify_all();

            std::unique_lock<std::mutex> lk(batch->mutex);
            batch->cv.wait(lk, [&] { return batch->done >= batch->expected; });
            dStart = std::move(batch->dStart);
            dEnd = std::move(batch->dEnd);
            ok = std::move(batch->ok);
            return batch->allOk;
        }

        bool submitAsync(const std::shared_ptr<StreamingSplitDecodeState>& state, int slotIndex) {
            if (!state || slotIndex < 0 || slotIndex >= state->expectedParts || slotIndex >= kMaxWorkers) return false;
            if (!state->frame.pixelsBGRA || state->fullPitch <= 0) return false;
            const auto& part = state->parts[static_cast<size_t>(slotIndex)];
            if (part.skipped || part.jpeg.empty()) return false;
            ensureStarted();

            WorkItem wi;
            wi.part = &state->parts[static_cast<size_t>(slotIndex)];
            wi.dst = state->frame.pixelsBGRA->data() + static_cast<size_t>(part.partTop) * static_cast<size_t>(state->fullPitch) + static_cast<size_t>(part.partLeft) * 4u;
            wi.pitch = state->fullPitch;
            wi.streamState = state;
            wi.slotIndex = slotIndex;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                queue_.push_back(std::move(wi));
            }
            cv_.notify_one();
            return true;
        }

        void stop() {
            bool expected = false;
            if (!stopping_.compare_exchange_strong(expected, true)) return;
            cv_.notify_all();
            for (auto& th : workers_) {
                if (th.joinable()) th.join();
            }
        }

    private:
        enum { kMaxWorkers = MAX_RUNTIME_SPLIT_PARTS };

        struct SyncBatch {
            std::mutex mutex;
            std::condition_variable cv;
            int expected = 0;
            int done = 0;
            bool allOk = true;
            std::vector<int64_t> dStart;
            std::vector<int64_t> dEnd;
            std::vector<int> ok;
        };

        struct WorkItem {
            const CompressedFramePart* part = nullptr;
            uint8_t* dst = nullptr;
            int pitch = 0;
            std::shared_ptr<SyncBatch> syncBatch;
            std::shared_ptr<StreamingSplitDecodeState> streamState;
            int slotIndex = -1;
        };

        std::once_flag once_;
        std::atomic<bool> stopping_{false};
        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<WorkItem> queue_;
        std::thread workers_[kMaxWorkers];

        void ensureStarted() {
            std::call_once(once_, [this] {
                for (int i = 0; i < kMaxWorkers; ++i) {
                    workers_[i] = std::thread([this, i] { workerLoop(i); });
                }
            });
        }

        void workerLoop(int /*index*/) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            tjhandle tj = tjInitDecompress();
            if (!tj) return;
            std::vector<uint8_t> scaledDecodeTmp;
            for (;;) {
                WorkItem item;
                {
                    std::unique_lock<std::mutex> lk(mutex_);
                    cv_.wait(lk, [&] { return stopping_.load() || !queue_.empty(); });
                    if (stopping_.load() && queue_.empty()) break;
                    item = std::move(queue_.front());
                    queue_.pop_front();
                }

                int64_t startNs = NowNs();
                int rc = -1;
                if (item.part && item.dst && item.pitch > 0 && !item.part->jpeg.empty()) {
                    const int encodedW = item.part->header.width;
                    const int encodedH = item.part->header.height;
                    const int displayW = item.part->partWidth > 0 ? item.part->partWidth : encodedW;
                    const int displayH = item.part->partHeight > 0 ? item.part->partHeight : encodedH;
                    if (encodedW == displayW && encodedH == displayH) {
                        rc = tjDecompress2(
                            tj,
                            item.part->jpeg.data(),
                            static_cast<unsigned long>(item.part->jpeg.size()),
                            item.dst,
                            displayW,
                            item.pitch,
                            displayH,
                            TJPF_BGRX,
                            TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE
                        );
                    } else if (encodedW > 0 && encodedH > 0 && displayW > 0 && displayH > 0) {
                        const int tmpPitch = encodedW * 4;
                        const size_t tmpBytes = static_cast<size_t>(encodedH) * static_cast<size_t>(tmpPitch);
                        if (scaledDecodeTmp.size() < tmpBytes) scaledDecodeTmp.resize(tmpBytes);
                        rc = tjDecompress2(
                            tj,
                            item.part->jpeg.data(),
                            static_cast<unsigned long>(item.part->jpeg.size()),
                            scaledDecodeTmp.data(),
                            encodedW,
                            tmpPitch,
                            encodedH,
                            TJPF_BGRX,
                            TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE
                        );
                        if (rc == 0) {
                            for (int y = 0; y < displayH; ++y) {
                                int sy = static_cast<int>((static_cast<int64_t>(y) * encodedH + displayH / 2) / displayH);
                                if (sy >= encodedH) sy = encodedH - 1;
                                const uint8_t* srcRow = scaledDecodeTmp.data() + static_cast<size_t>(sy) * static_cast<size_t>(tmpPitch);
                                uint8_t* dstRow = item.dst + static_cast<size_t>(y) * static_cast<size_t>(item.pitch);
                                for (int x = 0; x < displayW; ++x) {
                                    int sx = static_cast<int>((static_cast<int64_t>(x) * encodedW + displayW / 2) / displayW);
                                    if (sx >= encodedW) sx = encodedW - 1;
                                    const uint8_t* src = srcRow + static_cast<size_t>(sx) * 4u;
                                    uint8_t* dst = dstRow + static_cast<size_t>(x) * 4u;
                                    dst[0] = src[0];
                                    dst[1] = src[1];
                                    dst[2] = src[2];
                                    dst[3] = src[3];
                                }
                            }
                        }
                    }
                }
                int64_t endNs = NowNs();
                const bool partOk = (rc == 0);

                if (item.streamState) {
                    auto st = item.streamState;
                    std::lock_guard<std::mutex> lk(st->mutex);
                    if (item.slotIndex >= 0 && item.slotIndex < static_cast<int>(st->dStart.size())) {
                        st->dStart[static_cast<size_t>(item.slotIndex)] = startNs;
                        st->dEnd[static_cast<size_t>(item.slotIndex)] = endNs;
                        st->ok[static_cast<size_t>(item.slotIndex)] = partOk ? 1 : 0;
                    }
                    if (startNs > 0 && (st->decodeFirstStartNs == 0 || startNs < st->decodeFirstStartNs)) {
                        st->decodeFirstStartNs = startNs;
                    }
                    if (endNs > st->decodeLastEndNs) {
                        st->decodeLastEndNs = endNs;
                    }
                    if (startNs > 0 && endNs >= startNs) {
                        const int64_t partNs = endNs - startNs;
                        st->decodeCpuSumNs += partNs;
                        if (partNs > st->decodeMaxPartNs) st->decodeMaxPartNs = partNs;
                    }
                    if (!partOk) st->failed = true;
                    ++st->decodedParts;
                    st->cv.notify_one();
                }

                if (item.syncBatch) {
                    auto batch = item.syncBatch;
                    std::lock_guard<std::mutex> lk(batch->mutex);
                    if (item.slotIndex >= 0 && item.slotIndex < static_cast<int>(batch->dStart.size())) {
                        batch->dStart[static_cast<size_t>(item.slotIndex)] = startNs;
                        batch->dEnd[static_cast<size_t>(item.slotIndex)] = endNs;
                        batch->ok[static_cast<size_t>(item.slotIndex)] = partOk ? 1 : 0;
                    }
                    batch->allOk = batch->allOk && partOk;
                    ++batch->done;
                    batch->cv.notify_one();
                }
            }
            tjDestroy(tj);
        }
    };

    // ---------- 常量 ----------
    static constexpr size_t kMaxPendingCompressedFrames = 2;
    static constexpr size_t kMaxPendingSplitPublishes = 4;
    static constexpr int kBgraRingSize = 4;

    // ---------- 成员变量 ----------
    SharedState& state_;
    HWND hwnd_{};
    int port_ = PORT;
    std::thread netThread_;
    std::thread decodeThread_;
    std::thread splitPublishThread_;
    SplitDecodePool splitDecodePool_;
    std::atomic<bool> stopped_{false};
    SOCKET sock_ = INVALID_SOCKET;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<PendingCompressedFrame> queue_;

    std::mutex splitPublishMutex_;
    std::condition_variable splitPublishCv_;
    std::deque<std::shared_ptr<StreamingSplitDecodeState>> splitPublishQueue_;

    std::atomic<uint64_t> queuedSeq_{0};
    std::atomic<uint64_t> streamEpoch_{1};
    std::atomic<uint64_t> generation_{0};
    int recvCount_ = 0;
    uint64_t recvBytes_ = 0;
    uint64_t recvPart0Bytes_ = 0;
    uint64_t recvPart1Bytes_ = 0;
    int recvLastParts_ = 1;
    std::chrono::steady_clock::time_point recvWindowStart_{std::chrono::steady_clock::now()};
    int decodedCount_ = 0;
    std::chrono::steady_clock::time_point decodedWindowStart_{std::chrono::steady_clock::now()};

    std::mutex bgraPoolMutex_;
    std::shared_ptr<std::vector<uint8_t>> bgraRing_[kBgraRingSize];
    int bgraRingCursor_ = 0;

    std::mutex lastDecodedFrameMutex_;
    std::shared_ptr<std::vector<uint8_t>> lastDecodedBgra_;
    int lastDecodedWidth_ = 0;
    int lastDecodedHeight_ = 0;

    // ---------- 辅助函数 ----------
    std::shared_ptr<std::vector<uint8_t>> acquireBgraBuffer(size_t needBytes) {
        if (needBytes == 0) return std::make_shared<std::vector<uint8_t>>();
        std::lock_guard<std::mutex> lk(bgraPoolMutex_);
        for (int attempt = 0; attempt < kBgraRingSize; ++attempt) {
            const int idx = (bgraRingCursor_ + attempt) % kBgraRingSize;
            auto& buf = bgraRing_[idx];
            if (!buf || buf.use_count() == 1) {
                if (!buf) buf = std::make_shared<std::vector<uint8_t>>();
                if (buf->capacity() < needBytes) buf->reserve(needBytes);
                buf->resize(needBytes);
                bgraRingCursor_ = (idx + 1) % kBgraRingSize;
                return buf;
            }
        }
        auto fresh = std::make_shared<std::vector<uint8_t>>(needBytes);
        bgraRing_[bgraRingCursor_] = fresh;
        bgraRingCursor_ = (bgraRingCursor_ + 1) % kBgraRingSize;
        return fresh;
    }

    void rememberDecodedFrameBuffer(const DecodedFrame& frame) {
        std::lock_guard<std::mutex> lk(lastDecodedFrameMutex_);
        lastDecodedBgra_ = frame.pixelsBGRA;
        lastDecodedWidth_ = frame.width;
        lastDecodedHeight_ = frame.height;
    }

    void signalIncomingGeometryChanged(int width, int height) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(lastDecodedFrameMutex_);
            changed = lastDecodedWidth_ > 0 && lastDecodedHeight_ > 0 &&
                      (lastDecodedWidth_ != width || lastDecodedHeight_ != height);
            if (changed) {
                lastDecodedBgra_.reset();
                lastDecodedWidth_ = 0;
                lastDecodedHeight_ = 0;
            }
        }
        if (!changed) return;
        {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.latest = DecodedFrame{};
            state_.hasFrame = false;
            ++state_.streamResetGeneration;
        }
        if (state_.frameReadyEvent) SetEvent(state_.frameReadyEvent);
    }

    void setStatus(const wchar_t* text) {
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.status = text;
    }

    void updateRecvFpsCounter(uint64_t jpegBytes, uint64_t part0Bytes, uint64_t part1Bytes, int parts) {
        recvCount_++;
        recvBytes_ += jpegBytes;
        recvPart0Bytes_ += part0Bytes;
        recvPart1Bytes_ += part1Bytes;
        recvLastParts_ = (std::max)(1, parts);
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - recvWindowStart_).count();
        if (elapsed >= 1.0) {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.recvFps = recvCount_ / elapsed;
            state_.recvMbps = (double(recvBytes_) * 8.0 / 1000000.0) / elapsed;
            state_.avgJpegKb = recvCount_ > 0 ? double(recvBytes_) / 1024.0 / double(recvCount_) : 0.0;
            state_.avgPart0Kb = recvCount_ > 0 ? double(recvPart0Bytes_) / 1024.0 / double(recvCount_) : 0.0;
            state_.avgPart1Kb = recvCount_ > 0 ? double(recvPart1Bytes_) / 1024.0 / double(recvCount_) : 0.0;
            state_.recvParts = recvLastParts_;
            recvCount_ = 0;
            recvBytes_ = 0;
            recvPart0Bytes_ = 0;
            recvPart1Bytes_ = 0;
            recvWindowStart_ = now;
        }
    }

    void updateDecodedFpsCounter() {
        decodedCount_++;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - decodedWindowStart_).count();
        if (elapsed >= 1.0) {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.decodeFps = decodedCount_ / elapsed;
            decodedCount_ = 0;
            decodedWindowStart_ = now;
        }
    }

    void enqueueCompressedFrame(PendingCompressedFrame&& item) {
        {
            std::lock_guard<std::mutex> lk(queueMutex_);
            if (queue_.size() >= kMaxPendingCompressedFrames) {
                queue_.pop_front();
            }
            queuedSeq_.store(item.seq, std::memory_order_release);
            queue_.push_back(std::move(item));
        }
        queueCv_.notify_one();
    }

    bool dequeueCompressedFrame(PendingCompressedFrame& out) {
        std::unique_lock<std::mutex> lk(queueMutex_);
        queueCv_.wait(lk, [this] {
            return stopped_.load() || state_.stop.load() || !queue_.empty();
        });
        if (stopped_.load() || state_.stop.load()) return false;
        if (queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void publishFrame(DecodedFrame&& frame) {
        rememberDecodedFrameBuffer(frame);
        {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.latest = std::move(frame);
            state_.hasFrame = true;
        }
        if (state_.frameReadyEvent) {
            SetEvent(state_.frameReadyEvent);
        }
    }

    void clearCompressedQueue() {
        std::lock_guard<std::mutex> lk(queueMutex_);
        queue_.clear();
    }

    void clearSplitPublishQueue() {
        std::lock_guard<std::mutex> lk(splitPublishMutex_);
        splitPublishQueue_.clear();
    }

    void enqueueSplitForPublish(const std::shared_ptr<StreamingSplitDecodeState>& st) {
        if (!st) return;
        {
            std::lock_guard<std::mutex> lk(splitPublishMutex_);
            while (splitPublishQueue_.size() >= kMaxPendingSplitPublishes) {
                splitPublishQueue_.pop_front();
            }
            splitPublishQueue_.push_back(st);
        }
        splitPublishCv_.notify_one();
    }

    bool dequeueSplitForPublish(std::shared_ptr<StreamingSplitDecodeState>& out) {
        std::unique_lock<std::mutex> lk(splitPublishMutex_);
        splitPublishCv_.wait(lk, [this] {
            return stopped_.load() || state_.stop.load() || !splitPublishQueue_.empty();
        });
        if (stopped_.load() || state_.stop.load()) return false;
        if (splitPublishQueue_.empty()) return false;
        out = std::move(splitPublishQueue_.front());
        splitPublishQueue_.pop_front();
        return true;
    }

    void publishStreamingSplitState(const std::shared_ptr<StreamingSplitDecodeState>& st) {
        if (!st) return;

        {
            std::unique_lock<std::mutex> lk(st->mutex);
            while (!stopped_.load() && !state_.stop.load() && st->decodedParts < st->expectedParts) {
                st->cv.wait_for(lk, std::chrono::milliseconds(1));
            }
        }
        if (stopped_.load() || state_.stop.load()) return;
        if (st->streamEpoch != streamEpoch_.load(std::memory_order_acquire)) return;

        if (st->failed || !st->frame.pixelsBGRA) {
            setStatus(L"split part decode/reuse failed");
            return;
        }

        int64_t decodeBeginNs = st->decodeFirstStartNs;
        int64_t decodeDoneNs = st->decodeLastEndNs;
        int64_t decodeCpuSumNs = st->decodeCpuSumNs;
        int64_t decodeMaxPartNs = st->decodeMaxPartNs;
        if (decodeBeginNs == 0 || decodeDoneNs == 0 || decodeCpuSumNs == 0 || decodeMaxPartNs == 0) {
            decodeBeginNs = 0;
            decodeDoneNs = 0;
            decodeCpuSumNs = 0;
            decodeMaxPartNs = 0;
            for (int i = 0; i < st->expectedParts; ++i) {
                const int64_t ds = st->dStart[static_cast<size_t>(i)];
                const int64_t de = st->dEnd[static_cast<size_t>(i)];
                if (ds > 0 && (decodeBeginNs == 0 || ds < decodeBeginNs)) decodeBeginNs = ds;
                if (de > decodeDoneNs) decodeDoneNs = de;
                if (ds > 0 && de >= ds) {
                    const int64_t partNs = de - ds;
                    decodeCpuSumNs += partNs;
                    if (partNs > decodeMaxPartNs) decodeMaxPartNs = partNs;
                }
            }
        }
        if (decodeBeginNs == 0 && decodeDoneNs > 0) decodeBeginNs = decodeDoneNs;
        if (decodeDoneNs == 0 && decodeBeginNs > 0) decodeDoneNs = decodeBeginNs;
        if (decodeDoneNs < decodeBeginNs) decodeDoneNs = decodeBeginNs;

        {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.latestPartStatCount = (std::min)(st->expectedParts, MAX_RUNTIME_SPLIT_PARTS);
            if (st->availableEncodeCpuCount > 0) {
                state_.availableEncodeCpuCount = (std::min)(st->availableEncodeCpuCount, MAX_RUNTIME_SPLIT_PARTS);
            }
            for (int pi = 0; pi < MAX_RUNTIME_SPLIT_PARTS; ++pi) {
                state_.latestPartKb[pi] = 0.0;
                state_.latestPartMs[pi] = 0.0;
                state_.latestPartCpu[pi] = -1;
                state_.latestPartCpuFreqKhz[pi] = 0;
                state_.latestPartLeft[pi] = 0;
                state_.latestPartTop[pi] = 0;
                state_.latestPartWidth[pi] = 0;
                state_.latestPartHeight[pi] = 0;
                state_.latestPartSharePermille[pi] = 0;
            }
            for (int pi = 0; pi < state_.latestPartStatCount && pi < static_cast<int>(st->parts.size()); ++pi) {
                const auto& sp = st->parts[static_cast<size_t>(pi)];
                state_.latestPartKb[pi] = double(sp.jpeg.size()) / 1024.0;
                state_.latestPartMs[pi] = sp.partEncodeUs > 0 ? double(sp.partEncodeUs) / 1000.0 : 0.0;
                state_.latestPartCpu[pi] = sp.partCpuId;
                state_.latestPartCpuFreqKhz[pi] = sp.partCpuFreqKhz;
                state_.latestPartLeft[pi] = sp.partLeft;
                state_.latestPartTop[pi] = sp.partTop;
                state_.latestPartWidth[pi] = sp.partWidth;
                state_.latestPartHeight[pi] = sp.partHeight;
                state_.latestPartSharePermille[pi] = sp.partSharePermille > 0
                    ? sp.partSharePermille
                    : ((sp.partWidth > 0 && sp.fullWidth > 0 && sp.partWidth != sp.fullWidth)
                        ? int((int64_t(sp.partWidth) * 1000 + sp.fullWidth / 2) / sp.fullWidth)
                        : (sp.fullHeight > 0 ? int((int64_t(sp.partHeight) * 1000 + sp.fullHeight / 2) / sp.fullHeight) : 0));
            }
        }

        DecodedFrame frame = std::move(st->frame);
        frame.captureMs = (std::max)(0.0, double(st->header.callbackStartNs - st->header.frameProducedNs) / 1000000.0);
        frame.encodeMs = (std::max)(0.0, double(st->header.encodeEndNs - st->header.encodeStartNs) / 1000000.0);
        frame.queueMs = (std::max)(0.0, double(st->header.sendStartNs - st->header.encodeEndNs) / 1000000.0);
        frame.socketMs = (std::max)(0.0, double(st->recvDoneNs - st->recvBeginNs) / 1000000.0);
        frame.decodeMs = (std::max)(0.0, double(decodeDoneNs - decodeBeginNs) / 1000000.0);
        frame.decodeCpuSumMs = decodeCpuSumNs > 0 ? double(decodeCpuSumNs) / 1000000.0 : frame.decodeMs;
        frame.decodeMaxPartMs = decodeMaxPartNs > 0 ? double(decodeMaxPartNs) / 1000000.0 : frame.decodeMs;
        frame.decodePartCount = (std::max)(1, st->expectedParts);
        frame.decodeTailWaitMs = (std::max)(0.0, frame.decodeMs - frame.decodeCpuSumMs / frame.decodePartCount);
        frame.decodeOverlapSavedMs = (std::max)(0.0, frame.decodeCpuSumMs - frame.decodeMs);
        frame.generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        frame.frameProducedNs = st->header.frameProducedNs;

        if (g_perfLog.isRecording()) {
            for (int pi = 0; pi < st->expectedParts && pi < static_cast<int>(st->parts.size()); ++pi) {
                const auto& sp = st->parts[static_cast<size_t>(pi)];
                const int64_t ds = pi < static_cast<int>(st->dStart.size()) ? st->dStart[static_cast<size_t>(pi)] : 0;
                const int64_t de = pi < static_cast<int>(st->dEnd.size()) ? st->dEnd[static_cast<size_t>(pi)] : 0;
                const double partDecodeMs = (ds > 0 && de >= ds) ? double(de - ds) / 1000000.0 : 0.0;
                const double partRecvReadMs = DiffMs(sp.recvDoneNs, sp.recvBeginNs);
                const double partEncodeMs = sp.partEncodeUs > 0 ? double(sp.partEncodeUs) / 1000.0 : 0.0;
                char partRow[1536]{};
                std::snprintf(partRow, sizeof(partRow),
                    "%llu,%llu,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,0,0,0,0,0,0,0,0,0,%d,%d,%.3f,0,0,%s;aworker=%d;send_order=%d;dispatch_wait_ms=%.3f;wait_before_write_ms=%.3f;write_before_ms=%.3f;prev_write_ms=%.3f;writer_cpu=%d;sender_state=%d;aflags=%d;pc_arrival_gap_ms=%.3f;pc_frame_recv_span_ms=%.3f",
                    static_cast<unsigned long long>(st->seq),
                    static_cast<unsigned long long>(frame.generation),
                    static_cast<long long>(st->header.frameProducedNs),
                    frame.width, frame.height, pi, st->expectedParts, st->fullWidth, st->fullHeight,
                    sp.partLeft, sp.partTop, sp.partWidth, sp.partHeight, sp.header.width, sp.header.height,
                    double(sp.jpeg.size()) / 1024.0,
                    DiffMs(sp.header.callbackStartNs, sp.header.frameProducedNs),
                    DiffMs(sp.header.encodeEndNs, sp.header.encodeStartNs),
                    DiffMs(sp.header.sendStartNs, sp.header.encodeEndNs),
                    frame.socketMs, partRecvReadMs, partDecodeMs, partDecodeMs, partDecodeMs,
                    sp.partCpuId, sp.partCpuFreqKhz, partEncodeMs,
                    sp.skipped ? "decode_part_skip" : "decode_part",
                    sp.androidWorkerId, sp.androidSendOrder,
                    double(sp.androidDispatchDelayUs) / 1000.0,
                    double(sp.androidWaitBeforeWriteUs) / 1000.0,
                    double(sp.androidWriteBeforeUs) / 1000.0,
                    double(sp.androidPreviousWriteUs) / 1000.0,
                    sp.androidWriterCpuId, sp.androidSenderState, sp.androidFlags,
                    sp.pcArrivalGapMs, sp.pcFrameRecvSpanMs);
                g_perfLog.recordRow("decode_part", partRow);
            }

            double maxWaitBeforeWriteMs = 0.0;
            double maxPrevWriteMs = 0.0;
            double maxWriteBeforeMs = 0.0;
            double maxPcArrivalGapMs = 0.0;
            double maxPcFrameRecvSpanMs = 0.0;
            int lastAndroidSendOrder = -1;
            int lastAndroidPart = -1;
            for (int pi = 0; pi < st->expectedParts && pi < static_cast<int>(st->parts.size()); ++pi) {
                const auto& sp = st->parts[static_cast<size_t>(pi)];
                const double waitMs = double(sp.androidWaitBeforeWriteUs) / 1000.0;
                const double prevWriteMs = double(sp.androidPreviousWriteUs) / 1000.0;
                const double writeBeforeMs = double(sp.androidWriteBeforeUs) / 1000.0;
                if (waitMs > maxWaitBeforeWriteMs) maxWaitBeforeWriteMs = waitMs;
                if (prevWriteMs > maxPrevWriteMs) maxPrevWriteMs = prevWriteMs;
                if (writeBeforeMs > maxWriteBeforeMs) maxWriteBeforeMs = writeBeforeMs;
                if (sp.pcArrivalGapMs > maxPcArrivalGapMs) maxPcArrivalGapMs = sp.pcArrivalGapMs;
                if (sp.pcFrameRecvSpanMs > maxPcFrameRecvSpanMs) maxPcFrameRecvSpanMs = sp.pcFrameRecvSpanMs;
                if (sp.androidSendOrder > lastAndroidSendOrder) {
                    lastAndroidSendOrder = sp.androidSendOrder;
                    lastAndroidPart = pi;
                }
            }

            char frameRow[1536]{};
            std::snprintf(frameRow, sizeof(frameRow),
                "%llu,%llu,%lld,%d,%d,-1,%d,%d,%d,0,0,%d,%d,%d,%d,%.1f,%.3f,%.3f,%.3f,%.3f,0,%.3f,%.3f,%.3f,%.3f,%.3f,0,0,0,0,0,0,0,-1,0,0,0,0,split_publish_async;max_wait_before_write_ms=%.3f;max_prev_write_ms=%.3f;max_write_before_ms=%.3f;max_pc_arrival_gap_ms=%.3f;pc_frame_recv_span_ms=%.3f;last_send_order=%d;last_send_part=%d",
                static_cast<unsigned long long>(st->seq),
                static_cast<unsigned long long>(frame.generation),
                static_cast<long long>(st->header.frameProducedNs),
                frame.width, frame.height, st->expectedParts, st->fullWidth, st->fullHeight,
                st->fullWidth, st->fullHeight, frame.width, frame.height, double(st->totalBytes) / 1024.0,
                frame.captureMs, frame.encodeMs, frame.queueMs, frame.socketMs, frame.decodeMs,
                frame.decodeCpuSumMs, frame.decodeMaxPartMs, frame.decodeTailWaitMs, frame.decodeOverlapSavedMs,
                maxWaitBeforeWriteMs, maxPrevWriteMs, maxWriteBeforeMs, maxPcArrivalGapMs,
                maxPcFrameRecvSpanMs, lastAndroidSendOrder, lastAndroidPart);
            g_perfLog.recordRow("publish_split", frameRow);
        }

        publishFrame(std::move(frame));
        updateDecodedFpsCounter();
        updateRecvFpsCounter(st->totalBytes, st->part0Bytes, st->part1Bytes, st->expectedParts);
    }

    void runSplitPublish() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        while (!stopped_.load() && !state_.stop.load()) {
            std::shared_ptr<StreamingSplitDecodeState> st;
            if (!dequeueSplitForPublish(st)) continue;
            publishStreamingSplitState(st);
        }
    }

    void resetPublishedStreamState(const wchar_t* statusText) {
        streamEpoch_.fetch_add(1, std::memory_order_acq_rel);
        clearSplitPublishQueue();
        {
            std::lock_guard<std::mutex> lk(lastDecodedFrameMutex_);
            lastDecodedBgra_.reset();
            lastDecodedWidth_ = 0;
            lastDecodedHeight_ = 0;
        }
        {
            std::lock_guard<std::mutex> lk(state_.mutex);
            state_.latest = DecodedFrame{};
            state_.hasFrame = false;
            state_.latestCenterRoi = DecodedFrame{};
            state_.hasCenterRoiFrame = false;
            state_.status = statusText ? statusText : L"disconnected";
            state_.recvFps = 0.0;
            state_.decodeFps = 0.0;
            state_.displayFps = 0.0;
            state_.recvMbps = 0.0;
            state_.avgJpegKb = 0.0;
            state_.avgPart0Kb = 0.0;
            state_.avgPart1Kb = 0.0;
            state_.recvParts = 1;
            state_.latestPartStatCount = 0;
            state_.availableEncodeCpuCount = 0;
            for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
                state_.latestPartKb[i] = 0.0;
                state_.latestPartMs[i] = 0.0;
                state_.latestPartCpu[i] = -1;
                state_.latestPartCpuFreqKhz[i] = 0;
                state_.latestPartTop[i] = 0;
                state_.latestPartHeight[i] = 0;
                state_.latestPartSharePermille[i] = 0;
            }
            ++state_.streamResetGeneration;
        }
        if (state_.frameReadyEvent) SetEvent(state_.frameReadyEvent);
    }

    // ---------- runNet() ----------
    void runNet() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        std::vector<uint8_t> headerBuf(68);
        std::vector<uint8_t> extraBuf(64);
        uint64_t seq = 0;
        std::shared_ptr<StreamingSplitDecodeState> streamingSplit;

        auto resetSplitFrame = [&]() {
            streamingSplit.reset();
        };

        auto markStreamingPartFinished = [&](const std::shared_ptr<StreamingSplitDecodeState>& st,
                                             int slotIndex,
                                             bool ok,
                                             int64_t startNs,
                                             int64_t endNs) {
            if (!st) return;
            std::lock_guard<std::mutex> lk(st->mutex);
            if (slotIndex >= 0 && slotIndex < static_cast<int>(st->dStart.size())) {
                st->dStart[static_cast<size_t>(slotIndex)] = startNs;
                st->dEnd[static_cast<size_t>(slotIndex)] = endNs;
                st->ok[static_cast<size_t>(slotIndex)] = ok ? 1 : 0;
            }
            if (startNs > 0 && (st->decodeFirstStartNs == 0 || startNs < st->decodeFirstStartNs)) {
                st->decodeFirstStartNs = startNs;
            }
            if (endNs > st->decodeLastEndNs) {
                st->decodeLastEndNs = endNs;
            }
            if (startNs > 0 && endNs >= startNs) {
                const int64_t partNs = endNs - startNs;
                st->decodeCpuSumNs += partNs;
                if (partNs > st->decodeMaxPartNs) st->decodeMaxPartNs = partNs;
            }
            if (!ok) st->failed = true;
            ++st->decodedParts;
            st->cv.notify_one();
        };

        auto copySkippedPartFromLastFrame = [&](const std::shared_ptr<StreamingSplitDecodeState>& st,
                                                int slotIndex) -> bool {
            if (!st || slotIndex < 0 || slotIndex >= st->expectedParts || !st->frame.pixelsBGRA) return false;
            const auto& part = st->parts[static_cast<size_t>(slotIndex)];
            const int64_t startNs = NowNs();

            std::shared_ptr<std::vector<uint8_t>> last;
            int lastW = 0;
            int lastH = 0;
            {
                std::lock_guard<std::mutex> lk(lastDecodedFrameMutex_);
                last = lastDecodedBgra_;
                lastW = lastDecodedWidth_;
                lastH = lastDecodedHeight_;
            }

            bool ok = false;
            if (last && lastW == st->fullWidth && lastH == st->fullHeight &&
                part.partLeft >= 0 && part.partWidth > 0 && part.partLeft + part.partWidth <= st->fullWidth &&
                part.partTop >= 0 && part.partHeight > 0 && part.partTop + part.partHeight <= st->fullHeight) {
                const size_t pitch = static_cast<size_t>(st->fullPitch);
                const size_t need = static_cast<size_t>(st->fullHeight) * pitch;
                if (last->size() >= need && st->frame.pixelsBGRA->size() >= need) {
                    const uint8_t* src = last->data() + static_cast<size_t>(part.partTop) * pitch + static_cast<size_t>(part.partLeft) * 4u;
                    uint8_t* dst = st->frame.pixelsBGRA->data() + static_cast<size_t>(part.partTop) * pitch + static_cast<size_t>(part.partLeft) * 4u;
                    const size_t rowBytes = static_cast<size_t>(part.partWidth) * 4u;
                    for (int yy = 0; yy < part.partHeight; ++yy) {
                        std::memcpy(dst + static_cast<size_t>(yy) * pitch, src + static_cast<size_t>(yy) * pitch, rowBytes);
                    }
                    ok = true;
                }
            }

            const int64_t endNs = NowNs();
            markStreamingPartFinished(st, slotIndex, ok, startNs, endNs);
            return ok;
        };

        while (!stopped_.load() && !state_.stop.load()) {
            // ---- 启动 TCP 服务器监听，等待 Android 设备连接 ----
            std::wstring err;
            SOCKET listenSock = StartTcpServer(port_, err);
            if (listenSock == INVALID_SOCKET) {
                setStatus((L"TCP Server error: " + err).c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            setStatus(L"Waiting for Android device to connect...");

            SOCKET clientSock = AcceptTcpClient(listenSock, err);
            closesocket(listenSock);   // 只接受一个连接
            if (clientSock == INVALID_SOCKET) {
                setStatus((L"Accept error: " + err).c_str());
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
            setStatus(L"Connected");

            // ---- 连接建立，开始接收视频流 ----
            sock_ = clientSock;
            resetSplitFrame();
            {
                std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
                state_.controlSocket = clientSock;
            }
            seq = 0;
            queuedSeq_.store(0, std::memory_order_release);
            recvCount_ = 0;
            recvBytes_ = 0;
            recvPart0Bytes_ = 0;
            recvPart1Bytes_ = 0;
            recvLastParts_ = 1;
            decodedCount_ = 0;
            recvWindowStart_ = std::chrono::steady_clock::now();
            decodedWindowStart_ = std::chrono::steady_clock::now();
            resetSplitFrame();
            const uint64_t connectionEpoch = streamEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;

            {
                const int actualRcv = GetSocketOptInt(clientSock, SOL_SOCKET, SO_RCVBUF);
                const int actualSnd = GetSocketOptInt(clientSock, SOL_SOCKET, SO_SNDBUF);
                std::wstring st = L"Connected, rcvbuf=" + std::to_wstring(actualRcv / 1024) + L"KB, sndbuf=" + std::to_wstring(actualSnd / 1024) + L"KB";
                setStatus(st.c_str());
            }
            BufferedSocketReader videoReader(clientSock, 1024 * 1024);

            // ---- 主接收循环（完全来自原始代码） ----
            while (!stopped_.load() && !state_.stop.load()) {
                if (!videoReader.readAll(headerBuf.data(), static_cast<int>(headerBuf.size()))) break;
                FrameHeader h = ParseHeader(headerBuf.data());
                const bool isSplitVersion = (h.version == 3 || h.version == 4 || h.version == 5 || h.version == 6 || h.version == 7 || h.version == 8 || h.version == 9);
                const bool isSingleVersion = (h.version == 2);

                if (h.magic != static_cast<int32_t>(FRAME_MAGIC) || h.width <= 0 || h.height <= 0 ||
                    h.jpegSize < 0 || h.jpegSize > 20 * 1024 * 1024 ||
                    (!isSingleVersion && !isSplitVersion) ||
                    (isSingleVersion && h.jpegSize <= 0) ||
                    (h.jpegSize == 0 && h.version != 6 && h.version != 7 && h.version != 8 && h.version != 9)) {
                    break;
                }

                if (isSplitVersion) {
                    // ---------- 分块帧处理 ----------
                    const int extraSize = (h.version >= 9) ? 88 : ((h.version >= 7) ? 52 : ((h.version >= 5) ? 44 : ((h.version == 4) ? 40 : 32)));
                    if (static_cast<int>(extraBuf.size()) < extraSize) extraBuf.resize(static_cast<size_t>(extraSize));
                    if (!videoReader.readAll(extraBuf.data(), extraSize)) break;
                    const int partIndex = ReadBE32(extraBuf.data() + 0);
                    const int partCount = ReadBE32(extraBuf.data() + 4);
                    const int fullWidth = ReadBE32(extraBuf.data() + 8);
                    const int fullHeight = ReadBE32(extraBuf.data() + 12);
                    const int partLeft = (h.version >= 7) ? ReadBE32(extraBuf.data() + 16) : 0;
                    const int partTop = (h.version >= 7) ? ReadBE32(extraBuf.data() + 20) : ReadBE32(extraBuf.data() + 16);
                    const int partWidth = (h.version >= 7) ? ReadBE32(extraBuf.data() + 24) : fullWidth;
                    const int partHeight = (h.version >= 7) ? ReadBE32(extraBuf.data() + 28) : ReadBE32(extraBuf.data() + 20);
                    const int partEncodeUs = ReadBE32(extraBuf.data() + (h.version >= 7 ? 32 : 24));
                    const int partCpuId = ReadBE32(extraBuf.data() + (h.version >= 7 ? 36 : 28));
                    const int partCpuFreqKhz = (h.version >= 4) ? ReadBE32(extraBuf.data() + (h.version >= 7 ? 40 : 32)) : 0;
                    const int partSharePermille = (h.version >= 4) ? ReadBE32(extraBuf.data() + (h.version >= 7 ? 44 : 36)) : 0;
                    const int availableEncodeCpuCount = (h.version >= 5) ? ReadBE32(extraBuf.data() + (h.version >= 7 ? 48 : 40)) : 0;
                    const int androidWorkerId = (h.version >= 9) ? ReadBE32(extraBuf.data() + 52) : -1;
                    const int androidSendOrder = (h.version >= 9) ? ReadBE32(extraBuf.data() + 56) : -1;
                    const int androidDispatchDelayUs = (h.version >= 9) ? ReadBE32(extraBuf.data() + 60) : 0;
                    const int androidWaitBeforeWriteUs = (h.version >= 9) ? ReadBE32(extraBuf.data() + 64) : 0;
                    const int androidWriteBeforeUs = (h.version >= 9) ? ReadBE32(extraBuf.data() + 68) : 0;
                    const int androidPreviousWriteUs = (h.version >= 9) ? ReadBE32(extraBuf.data() + 72) : 0;
                    const int androidWriterCpuId = (h.version >= 9) ? ReadBE32(extraBuf.data() + 76) : -1;
                    const int androidSenderState = (h.version >= 9) ? ReadBE32(extraBuf.data() + 80) : 0;
                    const int androidFlags = (h.version >= 9) ? ReadBE32(extraBuf.data() + 84) : 0;

                    const bool encodedSizeOk = (h.version >= 8)
                        ? (h.width > 0 && h.height > 0 && h.width <= partWidth && h.height <= partHeight)
                        : (h.width == partWidth && h.height == partHeight);
                    if (partCount <= 1 || partCount > MAX_RUNTIME_SPLIT_PARTS || partIndex < 0 || partIndex >= partCount ||
                        fullWidth <= 0 || fullHeight <= 0 || partLeft < 0 || partTop < 0 || partWidth <= 0 || partHeight <= 0 ||
                        !encodedSizeOk ||
                        partLeft + partWidth > fullWidth || partTop + partHeight > fullHeight) {
                        break;
                    }

                    CompressedFramePart part;
                    part.header = h;
                    part.recvBeginNs = NowNs();
                    part.skipped = ((h.version == 6 || h.version == 7 || h.version == 8 || h.version == 9) && h.jpegSize == 0);
                    if (h.jpegSize > 0) {
                        part.jpeg.resize(static_cast<size_t>(h.jpegSize));
                        if (!videoReader.readAll(part.jpeg.data(), h.jpegSize)) break;
                    }
                    part.recvDoneNs = NowNs();
                    part.partIndex = partIndex;
                    part.partCount = partCount;
                    part.fullWidth = fullWidth;
                    part.fullHeight = fullHeight;
                    part.partLeft = partLeft;
                    part.partTop = partTop;
                    part.partWidth = partWidth;
                    part.partHeight = partHeight;
                    part.partEncodeUs = partEncodeUs;
                    part.partCpuId = partCpuId;
                    part.partCpuFreqKhz = partCpuFreqKhz;
                    part.partSharePermille = partSharePermille;
                    part.androidWorkerId = androidWorkerId;
                    part.androidSendOrder = androidSendOrder;
                    part.androidDispatchDelayUs = androidDispatchDelayUs;
                    part.androidWaitBeforeWriteUs = androidWaitBeforeWriteUs;
                    part.androidWriteBeforeUs = androidWriteBeforeUs;
                    part.androidPreviousWriteUs = androidPreviousWriteUs;
                    part.androidWriterCpuId = androidWriterCpuId;
                    part.androidSenderState = androidSenderState;
                    part.androidFlags = androidFlags;

                    if (!streamingSplit || streamingSplit->header.frameProducedNs != h.frameProducedNs) {
                        signalIncomingGeometryChanged(fullWidth, fullHeight);
                        streamingSplit = std::make_shared<StreamingSplitDecodeState>();
                        streamingSplit->streamEpoch = connectionEpoch;
                        streamingSplit->seq = ++seq;
                        streamingSplit->header = h;
                        streamingSplit->fullWidth = fullWidth;
                        streamingSplit->fullHeight = fullHeight;
                        streamingSplit->fullPitch = fullWidth * 4;
                        streamingSplit->expectedParts = partCount;
                        streamingSplit->availableEncodeCpuCount = availableEncodeCpuCount;
                        streamingSplit->frame.width = fullWidth;
                        streamingSplit->frame.height = fullHeight;
                        const size_t needBytes = static_cast<size_t>(fullHeight) * static_cast<size_t>(streamingSplit->fullPitch);
                        streamingSplit->frame.pixelsBGRA = acquireBgraBuffer(needBytes);
                        streamingSplit->parts.resize(static_cast<size_t>(partCount));
                        streamingSplit->received.assign(static_cast<size_t>(partCount), 0);
                        streamingSplit->dStart.assign(static_cast<size_t>(partCount), 0);
                        streamingSplit->dEnd.assign(static_cast<size_t>(partCount), 0);
                        streamingSplit->ok.assign(static_cast<size_t>(partCount), 0);
                        streamingSplit->recvBeginNs = part.recvBeginNs;
                        streamingSplit->recvDoneNs = part.recvDoneNs;
                    }

                    auto st = streamingSplit;
                    if (!st || st->streamEpoch != connectionEpoch || st->expectedParts != partCount ||
                        st->fullWidth != fullWidth || st->fullHeight != fullHeight ||
                        !st->frame.pixelsBGRA || st->frame.pixelsBGRA->size() < static_cast<size_t>(fullHeight) * static_cast<size_t>(fullWidth) * 4u ||
                        st->received[static_cast<size_t>(partIndex)] != 0) {
                        setStatus(L"split stream state invalid - frame dropped");
                        resetSplitFrame();
                        continue;
                    }

                    part.pcArrivalGapMs = (st->lastPartRecvDoneNs > 0)
                        ? DiffMs(part.recvDoneNs, st->lastPartRecvDoneNs)
                        : 0.0;
                    part.pcFrameRecvSpanMs = DiffMs(part.recvDoneNs, st->recvBeginNs);
                    st->lastPartRecvDoneNs = part.recvDoneNs;

                    if (g_perfLog.isRecording()) {
                        char row[1536]{};
                        const double jpegKb = double(part.jpeg.size()) / 1024.0;
                        const double androidCaptureMs = DiffMs(h.callbackStartNs, h.frameProducedNs);
                        const double androidEncodeMs = DiffMs(h.encodeEndNs, h.encodeStartNs);
                        const double androidQueueMs = DiffMs(h.sendStartNs, h.encodeEndNs);
                        const double recvReadMs = DiffMs(part.recvDoneNs, part.recvBeginNs);
                        const double partEncodeMs = partEncodeUs > 0 ? double(partEncodeUs) / 1000.0 : 0.0;
                        std::snprintf(row, sizeof(row),
                            "%llu,0,%lld,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.1f,%.3f,%.3f,%.3f,0,%.3f,0,0,0,0,0,0,0,0,0,0,0,0,%d,%d,%.3f,0,0,%s;aworker=%d;send_order=%d;dispatch_wait_ms=%.3f;wait_before_write_ms=%.3f;write_before_ms=%.3f;prev_write_ms=%.3f;writer_cpu=%d;sender_state=%d;aflags=%d;pc_arrival_gap_ms=%.3f;pc_frame_recv_span_ms=%.3f",
                            static_cast<unsigned long long>(st->seq),
                            static_cast<long long>(h.frameProducedNs),
                            fullWidth,
                            fullHeight,
                            partIndex,
                            partCount,
                            fullWidth,
                            fullHeight,
                            partLeft,
                            partTop,
                            partWidth,
                            partHeight,
                            h.width,
                            h.height,
                            jpegKb,
                            androidCaptureMs,
                            androidEncodeMs,
                            androidQueueMs,
                            recvReadMs,
                            partCpuId,
                            partCpuFreqKhz,
                            partEncodeMs,
                            part.skipped ? "split_part_skip" : "split_part",
                            part.androidWorkerId,
                            part.androidSendOrder,
                            double(part.androidDispatchDelayUs) / 1000.0,
                            double(part.androidWaitBeforeWriteUs) / 1000.0,
                            double(part.androidWriteBeforeUs) / 1000.0,
                            double(part.androidPreviousWriteUs) / 1000.0,
                            part.androidWriterCpuId,
                            part.androidSenderState,
                            part.androidFlags,
                            part.pcArrivalGapMs,
                            part.pcFrameRecvSpanMs);
                        g_perfLog.recordRow("recv_part", row);
                    }

                    if (h.encodeStartNs > 0 && (st->header.encodeStartNs == 0 || h.encodeStartNs < st->header.encodeStartNs)) st->header.encodeStartNs = h.encodeStartNs;
                    if (h.encodeEndNs > st->header.encodeEndNs) st->header.encodeEndNs = h.encodeEndNs;
                    if (h.sendStartNs > 0 && (st->header.sendStartNs == 0 || h.sendStartNs < st->header.sendStartNs)) st->header.sendStartNs = h.sendStartNs;
                    st->recvBeginNs = (std::min)(st->recvBeginNs, part.recvBeginNs);
                    st->recvDoneNs = (std::max)(st->recvDoneNs, part.recvDoneNs);
                    st->availableEncodeCpuCount = availableEncodeCpuCount > 0 ? availableEncodeCpuCount : st->availableEncodeCpuCount;

                    const int splitAt = (partCount + 1) / 2;
                    const uint64_t bytes = static_cast<uint64_t>(part.jpeg.size());
                    st->totalBytes += bytes;
                    if (part.partIndex < splitAt) st->part0Bytes += bytes;
                    else st->part1Bytes += bytes;

                    st->parts[static_cast<size_t>(partIndex)] = std::move(part);
                    st->received[static_cast<size_t>(partIndex)] = 1;
                    ++st->receivedParts;

                    if (st->parts[static_cast<size_t>(partIndex)].skipped) {
                        copySkippedPartFromLastFrame(st, partIndex);
                    } else if (!splitDecodePool_.submitAsync(st, partIndex)) {
                        const int64_t now = NowNs();
                        markStreamingPartFinished(st, partIndex, false, now, now);
                    }

                    if (st->receivedParts >= st->expectedParts) {
                        enqueueSplitForPublish(st);
                        resetSplitFrame();
                    }
                    continue;
                } else {
                    // ---------- 单帧处理 ----------
                    PendingCompressedFrame item;
                    item.header = h;
                    item.streamEpoch = connectionEpoch;
                    item.jpeg.resize(static_cast<size_t>(h.jpegSize));
                    item.recvBeginNs = NowNs();
                    if (!videoReader.readAll(item.jpeg.data(), h.jpegSize)) break;
                    item.recvDoneNs = NowNs();
                    item.seq = ++seq;

                    const uint64_t jpegBytes = static_cast<uint64_t>(item.jpeg.size());
                    enqueueCompressedFrame(std::move(item));
                    updateRecvFpsCounter(jpegBytes, jpegBytes, 0, 1);
                }
            }

            // ---- 断开连接清理 ----
            resetSplitFrame();
            {
                std::lock_guard<std::mutex> lk(state_.controlSocketMutex);
                if (state_.controlSocket == clientSock) state_.controlSocket = INVALID_SOCKET;
            }
            closesocket(clientSock);
            sock_ = INVALID_SOCKET;
            clearCompressedQueue();
            queueCv_.notify_all();
            resetPublishedStreamState(L"Device disconnected, re-listening...");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }

    // ---------- runDecode() ----------
    void runDecode() {
        tjhandle tj = tjInitDecompress();
        if (!tj) {
            setStatus(L"TurboJPEG init failed");
            return;
        }

        PendingCompressedFrame item;
        while (!stopped_.load() && !state_.stop.load()) {
            if (!dequeueCompressedFrame(item)) {
                continue;
            }
            if (item.streamEpoch != streamEpoch_.load(std::memory_order_acquire)) {
                continue;
            }

            DecodedFrame frame;
            int64_t decodeBeginNs = NowNs();
            int64_t decodeDoneNs = decodeBeginNs;
            bool decodeOk = false;

            if (item.split && item.parts.size() >= 2) {
                frame.width = item.fullWidth;
                frame.height = item.fullHeight;
                const int fullPitch = frame.width * 4;
                const size_t needBytes = static_cast<size_t>(frame.height) * static_cast<size_t>(fullPitch);
                frame.pixelsBGRA = acquireBgraBuffer(needBytes);

                const int partCount = static_cast<int>(item.parts.size());
                std::vector<int> ok;
                std::vector<int64_t> dStart;
                std::vector<int64_t> dEnd;
                decodeOk = splitDecodePool_.decode(item.parts, frame.pixelsBGRA->data(), fullPitch, dStart, dEnd, ok);

                int64_t decodeCpuSumNs = 0;
                int64_t decodeMaxPartNs = 0;
                if (!dStart.empty() && !dEnd.empty()) {
                    decodeBeginNs = dStart[0];
                    decodeDoneNs = dEnd[0];
                    for (int i = 0; i < partCount; ++i) {
                        const int64_t ds = dStart[static_cast<size_t>(i)];
                        const int64_t de = dEnd[static_cast<size_t>(i)];
                        if (ds > 0 && ds < decodeBeginNs) decodeBeginNs = ds;
                        if (de > decodeDoneNs) decodeDoneNs = de;
                        if (ds > 0 && de >= ds) {
                            const int64_t partNs = de - ds;
                            decodeCpuSumNs += partNs;
                            if (partNs > decodeMaxPartNs) decodeMaxPartNs = partNs;
                        }
                    }
                }
                frame.decodeCpuSumMs = decodeCpuSumNs > 0 ? double(decodeCpuSumNs) / 1000000.0 : 0.0;
                frame.decodeMaxPartMs = decodeMaxPartNs > 0 ? double(decodeMaxPartNs) / 1000000.0 : 0.0;
                frame.decodePartCount = partCount;
            } else {
                const int pitch = item.header.width * 4;
                const size_t needBytes = static_cast<size_t>(item.header.height) * static_cast<size_t>(pitch);

                frame.width = item.header.width;
                frame.height = item.header.height;
                frame.pixelsBGRA = acquireBgraBuffer(needBytes);

                decodeBeginNs = NowNs();
                int rc = tjDecompress2(
                    tj,
                    item.jpeg.data(),
                    static_cast<unsigned long>(item.jpeg.size()),
                    frame.pixelsBGRA->data(),
                    item.header.width,
                    pitch,
                    item.header.height,
                    TJPF_BGRX,
                    TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE
                );
                decodeDoneNs = NowNs();
                decodeOk = (rc == 0);
                frame.decodePartCount = 1;
            }

            if (!decodeOk) {
                setStatus(L"jpeg decode failed");
                continue;
            }
            if (item.streamEpoch != streamEpoch_.load(std::memory_order_acquire)) {
                continue;
            }

            frame.captureMs = (std::max)(0.0, double(item.header.callbackStartNs - item.header.frameProducedNs) / 1000000.0);
            frame.encodeMs = (std::max)(0.0, double(item.header.encodeEndNs - item.header.encodeStartNs) / 1000000.0);
            frame.queueMs = (std::max)(0.0, double(item.header.sendStartNs - item.header.encodeEndNs) / 1000000.0);
            frame.socketMs = (std::max)(0.0, double(item.recvDoneNs - item.recvBeginNs) / 1000000.0);
            frame.decodeMs = (std::max)(0.0, double(decodeDoneNs - decodeBeginNs) / 1000000.0);
            if (frame.decodeCpuSumMs <= 0.0) frame.decodeCpuSumMs = frame.decodeMs;
            if (frame.decodeMaxPartMs <= 0.0) frame.decodeMaxPartMs = frame.decodeMs;
            frame.decodeTailWaitMs =
                (std::max)(
                    0.0,
                    frame.decodeMs -
                    frame.decodeCpuSumMs / frame.decodePartCount
                );
            frame.decodeOverlapSavedMs = (std::max)(0.0, frame.decodeCpuSumMs - frame.decodeMs);
            frame.generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
            frame.frameProducedNs = item.header.frameProducedNs;

            if (g_perfLog.isRecording()) {
                uint64_t compressedBytes = static_cast<uint64_t>(item.jpeg.size());
                if (item.split) {
                    compressedBytes = 0;
                    for (const auto& sp : item.parts) compressedBytes += static_cast<uint64_t>(sp.jpeg.size());
                }
                const int partCount = item.split ? static_cast<int>(item.parts.size()) : 1;
                char frameRow[1024]{};
                std::snprintf(frameRow, sizeof(frameRow),
                    "%llu,%llu,%lld,%d,%d,-1,%d,%d,%d,0,0,%d,%d,%d,%d,%.1f,%.3f,%.3f,%.3f,%.3f,0,%.3f,%.3f,%.3f,%.3f,%.3f,0,0,0,0,0,0,0,-1,0,0,0,0,%s",
                    static_cast<unsigned long long>(item.seq),
                    static_cast<unsigned long long>(frame.generation),
                    static_cast<long long>(item.header.frameProducedNs),
                    frame.width,
                    frame.height,
                    (std::max)(1, partCount),
                    frame.width,
                    frame.height,
                    frame.width,
                    frame.height,
                    frame.width,
                    frame.height,
                    double(compressedBytes) / 1024.0,
                    frame.captureMs,
                    frame.encodeMs,
                    frame.queueMs,
                    frame.socketMs,
                    frame.decodeMs,
                    frame.decodeCpuSumMs,
                    frame.decodeMaxPartMs,
                    frame.decodeTailWaitMs,
                    frame.decodeOverlapSavedMs,
                    item.split ? "publish_split_queued" : "publish_single");
                g_perfLog.recordRow(item.split ? "publish_split_queued" : "publish_single", frameRow);
            }

            publishFrame(std::move(frame));
            updateDecodedFpsCounter();
        }

        tjDestroy(tj);
    }
};

// ---------- 外部接口 ----------
MirrorReceiver::MirrorReceiver(SharedState& state, int port)
    : impl_(std::make_unique<Impl>(state, port)) {}

MirrorReceiver::~MirrorReceiver() = default;

void MirrorReceiver::start() { impl_->start(); }
void MirrorReceiver::stop() { impl_->stop(); }