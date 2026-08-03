#pragma once
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <turbojpeg.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <algorithm>
#include <memory>
#include <utility>
#include <array>
#include "d3d11_shaders_embedded.h"
#include "mirror_types.h"
#include "hl_common.h"
#include "perf_logger.h"
#include "hud_overlay.h"
#include "mirror_receiver.h"
#include "protected_string.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

const wchar_t* WindowClassName();
const wchar_t* WindowTitleBase();

struct Vertex {
    float x, y, z;
    float u, v;
};
struct MirrorFrameViewport {
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
    int frameW = 0;
    int frameH = 0;
};

class D3D11Renderer {
public:
    explicit D3D11Renderer(HINSTANCE inst, SharedState& state);
    ~D3D11Renderer();
    bool init();
    ID3D11Device* d3dDevice() const;
    ID3D11DeviceContext* d3dContext() const;
    int run();
private:
    HINSTANCE inst_{};
    SharedState& state_;
    HWND hwnd_{};
    RECT windowedRect_{ 100, 100, 820, 320 };
    bool fullscreen_{ false };
    bool stretch_{ true };
    bool hudVisible_{ true };
    bool dirtyWindow_{ true };
    bool frameVerticesInitialized_{ false };
    bool windowSizedToFrame_{ false };
    bool allowTearing_{ false };
    ID3D11Device* device_{};
    std::chrono::steady_clock::time_point lastHudUpdate_{ std::chrono::steady_clock::now() - std::chrono::seconds(2) };
    std::wstring currentStatus_ = L"等待连接...";
    double cachedRecvFps_ = 0.0;
    double cachedDecodeFps_ = 0.0;
    double cachedDisplayFps_ = 0.0;
    double cachedRecvMbps_ = 0.0;
    double cachedAvgJpegKb_ = 0.0;
    double cachedPart0Kb_ = 0.0;
    double cachedPart1Kb_ = 0.0;
    int cachedRecvParts_ = 1;
    int cachedPartStatCount_ = 0;
    double cachedPartKb_[MAX_RUNTIME_SPLIT_PARTS]{};
    double cachedPartMs_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartCpu_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartCpuFreqKhz_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartLeft_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartTop_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartWidth_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartHeight_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedPartSharePermille_[MAX_RUNTIME_SPLIT_PARTS]{};
    int cachedAvailableEncodeCpuCount_ = 0;
    double cachedCaptureMs_ = 0.0;
    double cachedEncodeMs_ = 0.0;
    double cachedQueueMs_ = 0.0;
    double cachedSocketMs_ = 0.0;
    double cachedDecodeWallMs_ = 0.0;
    double cachedDecodeCpuSumMs_ = 0.0;
    double cachedDecodeMaxPartMs_ = 0.0;
    double cachedDecodeTailWaitMs_ = 0.0;
    double cachedDecodeOverlapSavedMs_ = 0.0;
    int cachedDecodePartCount_ = 1;
    uint64_t observedStreamResetGeneration_{ 0 };
    bool titleDirty_{ true };
    bool hudDirty_{ true };
    ID3D11DeviceContext* ctx_{};
    IDXGISwapChain1* swapChain_{};
    IDXGISwapChain2* swapChain2_{};
    ID3D11RenderTargetView* rtv_{};
    ID3D11VertexShader* vs_{};
    ID3D11PixelShader* ps_{};
    ID3D11BlendState* alphaBlend_{};
    ID3D11PixelShader* psFsrEasu_{ nullptr };
    ID3D11PixelShader* psFsrRcas_{ nullptr };
    ID3D11InputLayout* inputLayout_{ nullptr };
    ID3D11Buffer* vertexBuffer_{ nullptr };
    ID3D11Buffer* hudVertexBuffer_{ nullptr };
    ID3D11Buffer* fsrCb_{ nullptr };
    ID3D11SamplerState* sampler_{ nullptr };
    ID3D11Texture2D* fsrTex_{ nullptr };
    ID3D11ShaderResourceView* fsrSrv_{ nullptr };
    ID3D11RenderTargetView* fsrRtv_{ nullptr };
    ID3D11Texture2D* frameTex_{};
    ID3D11ShaderResourceView* frameSrv_{};
    ID3D11Texture2D* hudTex_{};
    ID3D11ShaderResourceView* hudSrv_{};
    std::vector<uint8_t> hudPixels_;
    int hudTexWidth_{ 1600 };
    int hudTexHeight_{ 720 };
    int hudUsedWidth_{ 0 };
    int hudUsedHeight_{ 0 };
    std::chrono::steady_clock::time_point presentStatsWindowStart_{ std::chrono::steady_clock::now() };
	std::chrono::steady_clock::time_point displayWindowStart_{ std::chrono::steady_clock::now() };
    int64_t lastPresentDoneNs_{ 0 };
    int64_t lastRenderBeginNs_{ 0 };
    double lastUploadCpuMs_{ 0.0 };
    int lastUploadMode_{ 0 };
    UINT lastUploadRowPitch_{ 0 };
    int uploadMapCountWindow_{ 0 };
    int uploadFallbackCountWindow_{ 0 };
    int uploadFailedCountWindow_{ 0 };
    int uploadMapCountShown_{ 0 };
    int uploadFallbackCountShown_{ 0 };
    int uploadFailedCountShown_{ 0 };
    double lastDrawCpuMs_{ 0.0 };
    double lastPresentCpuMs_{ 0.0 };
    double lowerBoundMs_{ 0.0 };
    double lowerBoundEqFps_{ 0.0 };
    double presentIntervalAvgMs_{ 0.0 };
    double presentIntervalMaxMs_{ 0.0 };
    double presentIntervalAccumMs_{ 0.0 };
    double presentIntervalLastMs_{ 0.0 };
    double presentIntervalShownAvgMs_{ 0.0 };
    double presentIntervalShownMaxMs_{ 0.0 };
    double presentIntervalP95Ms_{ 0.0 };
    double presentIntervalP99Ms_{ 0.0 };
    int presentIntervalSamples_{ 0 };
    std::vector<double> presentIntervalsWindow_{};
    int skippedFramesWindow_{ 0 };
    int skippedFramesLast_{ 0 };
    uint64_t uploadedGeneration_{ 0 };
    DecodedFrame currentFrame_;
    std::shared_ptr<GpuFrameResource> currentGpuFrame_;
    void updateStatusText(const std::wstring& text);
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    bool createWindow();
    bool createDevice();
    bool createShaders();
    bool createSampler();
    bool createHudResources();
	bool createBlendState();
    void drawHud(const D3D11_VIEWPORT& vp);
    void updateHudTextureIfNeeded(bool force);
    void drawChineseHudLines(const std::vector<std::wstring>& lines);
    void drawSplitDebugOverlay(uint8_t* bgra, int width, int height, int pitch);
    void appendPartStatsHudLines(std::vector<std::wstring>& lines) const;
    double totalHudKb() const;
    double totalHudMbps() const;
    void recreateRTV();
    bool ensureFrameTexture(int width, int height);
    bool uploadFrameTextureDynamic(const DecodedFrame& frame);
    void clearDisplayedFrameAfterStreamReset();
    bool consumeLatestFrame();
    void updateVertices();
    void render(bool presentedNewFrame);
    void toggleFullscreen();
    void fitWindowToFrameIfNeeded();
    void cleanup();
    int64_t absI64(int64_t v);
    void maybeUpdateWindowTitle(bool force);
    enum : int {
        IDC_STATIC_STATUS = 2001,
    };
};
