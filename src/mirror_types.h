#pragma once

#ifndef NOMINMAX
#define NOMINMAX
// ---------- 共享内存结构（供Python读取） ----------
#pragma pack(push, 1)
struct SharedFrameHeader {
    int32_t width;
    int32_t height;
    int32_t pitch;          // 每行字节数（一般为 width * 4）
    int64_t timestamp_ns;   // 帧产生时间（可选）
    uint32_t sequence;      // 帧序列号，递增
    uint32_t dataSize;      // 实际数据大小（width * height * 4）
};
#pragma pack(pop)

// 最大支持分辨率（可调，此处设为 1920x1080，若需要4K可改大）
static constexpr int SHARED_MEM_MAX_WIDTH  = 1920;
static constexpr int SHARED_MEM_MAX_HEIGHT = 1080;
static constexpr size_t SHARED_MEM_DATA_SIZE = 
    static_cast<size_t>(SHARED_MEM_MAX_WIDTH) * SHARED_MEM_MAX_HEIGHT * 4;
static constexpr size_t SHARED_MEM_TOTAL_SIZE = 
    sizeof(SharedFrameHeader) + SHARED_MEM_DATA_SIZE;
#endif

#include <winsock2.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

static constexpr int MAX_RUNTIME_SPLIT_PARTS = 24;

static constexpr int PORT = 27183;
static constexpr const char* SOCKET_NAME = "huilang_screen_mirror";
static constexpr uint32_t FRAME_MAGIC = 0x484C4D32;
static constexpr int64_t NS_PER_MS = 1000000LL;

struct FrameHeader {
    int32_t magic;
    int32_t version;
    int32_t width;
    int32_t height;
    int32_t jpegSize;
    int64_t frameProducedNs;
    int64_t callbackStartNs;
    int64_t encodeStartNs;
    int64_t encodeEndNs;
    int64_t sendStartNs;
    int64_t sendStartWallMs;
};

struct GpuFrameResource {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    GpuFrameResource() = default;
    GpuFrameResource(ID3D11Texture2D* t, ID3D11ShaderResourceView* s) : texture(t), srv(s) {
        if (texture) texture->AddRef();
        if (srv) srv->AddRef();
    }
    ~GpuFrameResource() {
        if (srv) srv->Release();
        if (texture) texture->Release();
    }
    GpuFrameResource(const GpuFrameResource&) = delete;
    GpuFrameResource& operator=(const GpuFrameResource&) = delete;
};

struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::shared_ptr<std::vector<uint8_t>> pixelsBGRA;
    std::shared_ptr<GpuFrameResource> gpuFrame;

    double captureMs = 0.0;
    double encodeMs = 0.0;
    double queueMs = 0.0;
    double socketMs = 0.0;
    double decodeMs = 0.0;
    double decodeCpuSumMs = 0.0;
    double decodeMaxPartMs = 0.0;
    double decodeTailWaitMs = 0.0;
    double decodeOverlapSavedMs = 0.0;
    int decodePartCount = 1;

    uint64_t generation = 0;
    int64_t frameProducedNs = 0;
};

struct SharedState {
    std::mutex mutex;
    DecodedFrame latest;
    bool hasFrame = false;
	DecodedFrame latestCenterRoi;
    bool hasCenterRoiFrame = false;
    std::wstring status = L"等待设备连接...";
    std::atomic<bool> stop{false};
    double recvFps = 0.0;
    double decodeFps = 0.0;
    double displayFps = 0.0;
    double recvMbps = 0.0;
    double avgJpegKb = 0.0;
    double avgPart0Kb = 0.0;
    double avgPart1Kb = 0.0;

    int recvParts = 1;
    int latestPartStatCount = 0;
    double latestPartKb[MAX_RUNTIME_SPLIT_PARTS]{};
    double latestPartMs[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartCpu[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartCpuFreqKhz[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartLeft[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartTop[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartWidth[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartHeight[MAX_RUNTIME_SPLIT_PARTS]{};
    int latestPartSharePermille[MAX_RUNTIME_SPLIT_PARTS]{};
    int availableEncodeCpuCount = 0;
    uint64_t streamResetGeneration = 0;
    HANDLE frameReadyEvent = nullptr;

    std::mutex controlSocketMutex;
    SOCKET controlSocket = INVALID_SOCKET;
};
