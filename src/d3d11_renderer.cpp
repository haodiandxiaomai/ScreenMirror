#include "d3d11_renderer.h"

#include <d3d11.h>
#include <dxgi1_3.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hl_common.h"
#include "mirror_net.h"      // for MAX_RUNTIME_SPLIT_PARTS
#include "perf_logger.h"


// ---------- 工具函数 ----------
static double percentileFromSorted(const std::vector<double>& values, double pct) {
    if (values.empty()) return 0.0;
    if (pct <= 0.0) return values.front();
    if (pct >= 100.0) return values.back();
    const double pos = (pct / 100.0) * double(values.size() - 1);
    const size_t lo = static_cast<size_t>(pos);
    const size_t hi = (std::min)(lo + 1, values.size() - 1);
    const double frac = pos - double(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}



// ---------- 窗口类名 ----------
const wchar_t* WindowClassName() { return L"ScreenMirrorWindow"; }
const wchar_t* WindowTitleBase() { return L"屏幕投屏"; }

// ---------- 构造函数 / 析构函数 ----------
D3D11Renderer::D3D11Renderer(HINSTANCE inst, SharedState& state)
    : inst_(inst), state_(state) {
    // 初始化成员变量
    hudVisible_ = true;
    stretch_ = true;
    fullscreen_ = false;
    allowTearing_ = false;
    frameVerticesInitialized_ = false;
    uploadedGeneration_ = 0;
    lastUploadCpuMs_ = 0.0;
    lastDrawCpuMs_ = 0.0;
    lastPresentCpuMs_ = 0.0;
    lowerBoundMs_ = 0.0;
    lowerBoundEqFps_ = 0.0;
    lastPresentDoneNs_ = 0;
    lastRenderBeginNs_ = 0;
    presentIntervalAvgMs_ = 0.0;
    presentIntervalMaxMs_ = 0.0;
    presentIntervalAccumMs_ = 0.0;
    presentIntervalLastMs_ = 0.0;
    presentIntervalShownAvgMs_ = 0.0;
    presentIntervalShownMaxMs_ = 0.0;
    presentIntervalP95Ms_ = 0.0;
    presentIntervalP99Ms_ = 0.0;
    presentIntervalSamples_ = 0;
    skippedFramesWindow_ = 0;
    skippedFramesLast_ = 0;
    uploadMapCountWindow_ = 0;
    uploadFallbackCountWindow_ = 0;
    uploadFailedCountWindow_ = 0;
    uploadMapCountShown_ = 0;
    uploadFallbackCountShown_ = 0;
    uploadFailedCountShown_ = 0;
    lastUploadMode_ = 0;
    lastUploadRowPitch_ = 0;
    dirtyWindow_ = true;
    hudDirty_ = true;
    titleDirty_ = true;
    windowSizedToFrame_ = false;
    observedStreamResetGeneration_ = 0;
    cachedRecvFps_ = 0.0;
    cachedDecodeFps_ = 0.0;
    cachedDisplayFps_ = 0.0;
    cachedRecvMbps_ = 0.0;
    cachedAvgJpegKb_ = 0.0;
    cachedPart0Kb_ = 0.0;
    cachedPart1Kb_ = 0.0;
    cachedRecvParts_ = 1;
    cachedPartStatCount_ = 0;
    cachedAvailableEncodeCpuCount_ = 0;
    std::memset(cachedPartKb_, 0, sizeof(cachedPartKb_));
    std::memset(cachedPartMs_, 0, sizeof(cachedPartMs_));
    std::memset(cachedPartCpu_, -1, sizeof(cachedPartCpu_));
    std::memset(cachedPartCpuFreqKhz_, 0, sizeof(cachedPartCpuFreqKhz_));
    std::memset(cachedPartLeft_, 0, sizeof(cachedPartLeft_));
    std::memset(cachedPartTop_, 0, sizeof(cachedPartTop_));
    std::memset(cachedPartWidth_, 0, sizeof(cachedPartWidth_));
    std::memset(cachedPartHeight_, 0, sizeof(cachedPartHeight_));
    std::memset(cachedPartSharePermille_, 0, sizeof(cachedPartSharePermille_));
    cachedCaptureMs_ = 0.0;
    cachedEncodeMs_ = 0.0;
    cachedQueueMs_ = 0.0;
    cachedSocketMs_ = 0.0;
    cachedDecodeWallMs_ = 0.0;
    cachedDecodeCpuSumMs_ = 0.0;
    cachedDecodeMaxPartMs_ = 0.0;
    cachedDecodeTailWaitMs_ = 0.0;
    cachedDecodeOverlapSavedMs_ = 0.0;
    cachedDecodePartCount_ = 1;

    // 重置统计窗口
    presentStatsWindowStart_ = std::chrono::steady_clock::now();
    displayWindowStart_ = presentStatsWindowStart_;

    // 指针置空
    device_ = nullptr;
    ctx_ = nullptr;
    swapChain_ = nullptr;
    swapChain2_ = nullptr;
    rtv_ = nullptr;
    vs_ = nullptr;
    ps_ = nullptr;
    psFsrEasu_ = nullptr;
    psFsrRcas_ = nullptr;
    inputLayout_ = nullptr;
    vertexBuffer_ = nullptr;
    hudVertexBuffer_ = nullptr;
    fsrCb_ = nullptr;
    sampler_ = nullptr;
    alphaBlend_ = nullptr;
    frameTex_ = nullptr;
    frameSrv_ = nullptr;
    hudTex_ = nullptr;
    hudSrv_ = nullptr;
    fsrTex_ = nullptr;
    fsrSrv_ = nullptr;
    fsrRtv_ = nullptr;
}

D3D11Renderer::~D3D11Renderer() { cleanup(); }

// ---------- 初始化 ----------
bool D3D11Renderer::init() {
    if (!createWindow()) return false;
    if (!createDevice()) return false;
    if (!createShaders()) return false;
    if (!createSampler()) return false;
    if (!createHudResources()) return false;
    if (!createBlendState()) return false;
    fullscreen_ = false;
    SetWindowLongW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
    dirtyWindow_ = true;
    hudDirty_ = true;
    titleDirty_ = true;
    return true;
}

ID3D11Device* D3D11Renderer::d3dDevice() const { return device_; }
ID3D11DeviceContext* D3D11Renderer::d3dContext() const { return ctx_; }

void D3D11Renderer::updateStatusText(const std::wstring& text) {
    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        state_.status = text;
    }
    currentStatus_ = text;
    hudDirty_ = true;
    titleDirty_ = true;
    dirtyWindow_ = true;
}

// ---------- 窗口创建 ----------
bool D3D11Renderer::createWindow() {
    WNDCLASS wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst_;
    wc.lpszClassName = WindowClassName();
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClass(&wc)) {
        // 若已注册则忽略
    }

    windowedRect_.left = 100;
    windowedRect_.top = 100;
    windowedRect_.right = 1100;
    windowedRect_.bottom = 700;

    hwnd_ = CreateWindowExW(   
        0,
        WindowClassName(),
        WindowTitleBase(),
        WS_OVERLAPPEDWINDOW,
        windowedRect_.left, windowedRect_.top,
        windowedRect_.right - windowedRect_.left,
        windowedRect_.bottom - windowedRect_.top,
        nullptr,
        nullptr,
        inst_,
        this
    );
    return hwnd_ != nullptr;
}

// ---------- D3D11 设备创建 ----------
bool D3D11Renderer::createDevice() {
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION,
        &device_, nullptr, &ctx_
    );
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory5* factory = nullptr;   // 改为 IDXGIFactory5

    hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->GetParent(__uuidof(IDXGIFactory5), (void**)&factory);  // 改为 IDXGIFactory5

    if (factory) {
        BOOL allowTearing = FALSE;
        factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
        allowTearing_ = (allowTearing == TRUE);
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 0;
    sd.Height = 0;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Stereo = FALSE;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_NONE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(device_, hwnd_, &sd, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) return false;

    if (factory) factory->Release();
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();

    swapChain_->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&swapChain2_);
    recreateRTV();
    return true;
}

void D3D11Renderer::recreateRTV() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    ID3D11Texture2D* backBuffer = nullptr;
    HRESULT hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (SUCCEEDED(hr) && backBuffer) {
        device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
        backBuffer->Release();
    }
}

// ---------- 着色器 ----------
bool D3D11Renderer::createShaders() {
    HRESULT hr;
    hr = device_->CreateVertexShader(g_MirrorVSBytecode, g_MirrorVSBytecodeSize, nullptr, &vs_);
    if (FAILED(hr)) return false;
    hr = device_->CreatePixelShader(g_MirrorPSBytecode, g_MirrorPSBytecodeSize, nullptr, &ps_);
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device_->CreateInputLayout(layout, 2, g_MirrorVSBytecode, g_MirrorVSBytecodeSize, &inputLayout_);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(Vertex) * 6;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&bd, nullptr, &vertexBuffer_);
    if (FAILED(hr)) return false;

    return true;
}

bool D3D11Renderer::createSampler() {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    return SUCCEEDED(device_->CreateSamplerState(&sd, &sampler_));
}

bool D3D11Renderer::createHudResources() {
    // HUD 纹理
    D3D11_TEXTURE2D_DESC td{};
    td.Width = hudTexWidth_;
    td.Height = hudTexHeight_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &hudTex_);
    if (FAILED(hr)) return false;
    hr = device_->CreateShaderResourceView(hudTex_, nullptr, &hudSrv_);
    if (FAILED(hr)) return false;

    // HUD 顶点缓冲
    Vertex v[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
    };
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(v);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init{ v, 0, 0 };
    hr = device_->CreateBuffer(&bd, &init, &hudVertexBuffer_);
    return SUCCEEDED(hr);
}

bool D3D11Renderer::createBlendState() {
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(device_->CreateBlendState(&bd, &alphaBlend_));
}

// ---------- 帧纹理管理 ----------
bool D3D11Renderer::ensureFrameTexture(int width, int height) {
    if (frameTex_ && currentFrame_.width == width && currentFrame_.height == height) return true;
    if (currentFrame_.width != width || currentFrame_.height != height) {
        windowSizedToFrame_ = false;
    }
    currentGpuFrame_.reset();
    if (frameSrv_) { frameSrv_->Release(); frameSrv_ = nullptr; }
    if (frameTex_) { frameTex_->Release(); frameTex_ = nullptr; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device_->CreateTexture2D(&td, nullptr, &frameTex_);
    if (FAILED(hr)) return false;
    hr = device_->CreateShaderResourceView(frameTex_, nullptr, &frameSrv_);
    return SUCCEEDED(hr);
}

bool D3D11Renderer::uploadFrameTextureDynamic(const DecodedFrame& frame) {
    if (!frameTex_ || frame.width <= 0 || frame.height <= 0 || (!frame.pixelsBGRA || frame.pixelsBGRA->empty())) {
        lastUploadMode_ = 3;
        lastUploadRowPitch_ = 0;
        ++uploadFailedCountWindow_;
        return false;
    }

    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx_->PSSetShaderResources(0, 1, nullSrv);

    const int srcPitch = frame.width * 4;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx_->Map(frameTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        lastUploadMode_ = 1;
        lastUploadRowPitch_ = mapped.RowPitch;
        ++uploadMapCountWindow_;

        uint8_t* srcMutable = frame.pixelsBGRA->data();
        drawSplitDebugOverlay(srcMutable, frame.width, frame.height, srcPitch);
        const uint8_t* src = srcMutable;
        uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
        if (mapped.RowPitch == static_cast<UINT>(srcPitch)) {
            std::memcpy(dst, src, static_cast<size_t>(srcPitch) * static_cast<size_t>(frame.height));
        } else {
            for (int y = 0; y < frame.height; ++y) {
                std::memcpy(
                    dst + static_cast<size_t>(y) * static_cast<size_t>(mapped.RowPitch),
                    src + static_cast<size_t>(y) * static_cast<size_t>(srcPitch),
                    static_cast<size_t>(srcPitch));
            }
        }
        ctx_->Unmap(frameTex_, 0);
        return true;
    }

    lastUploadMode_ = 2;
    lastUploadRowPitch_ = static_cast<UINT>(srcPitch);
    ++uploadFallbackCountWindow_;
    ctx_->UpdateSubresource(frameTex_, 0, nullptr, frame.pixelsBGRA->data(), srcPitch, 0);
    return true;
}

void D3D11Renderer::clearDisplayedFrameAfterStreamReset() {
    currentFrame_ = DecodedFrame{};
    uploadedGeneration_ = 0;
    lastUploadCpuMs_ = 0.0;
    lastDrawCpuMs_ = 0.0;
    lastPresentCpuMs_ = 0.0;
    lowerBoundMs_ = 0.0;
    lowerBoundEqFps_ = 0.0;
    lastPresentDoneNs_ = 0;
    lastRenderBeginNs_ = 0;
    presentIntervalAvgMs_ = 0.0;
    presentIntervalMaxMs_ = 0.0;
    presentIntervalAccumMs_ = 0.0;
    presentIntervalLastMs_ = 0.0;
    presentIntervalShownAvgMs_ = 0.0;
    presentIntervalShownMaxMs_ = 0.0;
    presentIntervalP95Ms_ = 0.0;
    presentIntervalP99Ms_ = 0.0;
    presentIntervalSamples_ = 0;
    presentIntervalsWindow_.clear();
    presentStatsWindowStart_ = std::chrono::steady_clock::now();
    skippedFramesWindow_ = 0;
    skippedFramesLast_ = 0;
    currentGpuFrame_.reset();
    if (frameSrv_) { frameSrv_->Release(); frameSrv_ = nullptr; }
    if (frameTex_) { frameTex_->Release(); frameTex_ = nullptr; }
    dirtyWindow_ = true;
    hudDirty_ = true;
    titleDirty_ = true;
}

// ---------- 帧消费 ----------
bool D3D11Renderer::consumeLatestFrame() {
    DecodedFrame newest;
    std::wstring status;
    double recvFps = 0.0;
    double decodeFps = 0.0;
    double displayFps = 0.0;
    double recvMbps = 0.0;
    double avgJpegKb = 0.0;
    double avgPart0Kb = 0.0;
    double avgPart1Kb = 0.0;
    int recvParts = 1;
    int partStatCount = 0;
    double partKb[MAX_RUNTIME_SPLIT_PARTS]{};
    double partMs[MAX_RUNTIME_SPLIT_PARTS]{};
    int partCpu[MAX_RUNTIME_SPLIT_PARTS]{};
    int partCpuFreqKhz[MAX_RUNTIME_SPLIT_PARTS]{};
    int partLeft[MAX_RUNTIME_SPLIT_PARTS]{};
    int partTop[MAX_RUNTIME_SPLIT_PARTS]{};
    int partWidth[MAX_RUNTIME_SPLIT_PARTS]{};
    int partHeight[MAX_RUNTIME_SPLIT_PARTS]{};
    int partSharePermille[MAX_RUNTIME_SPLIT_PARTS]{};
    int availableEncodeCpuCount = cachedAvailableEncodeCpuCount_;
    uint64_t streamResetGeneration = observedStreamResetGeneration_;
    bool gotFrame = false;

    {
        std::lock_guard<std::mutex> lk(state_.mutex);
        status = state_.status;
        recvFps = state_.recvFps;
        decodeFps = state_.decodeFps;
        displayFps = state_.displayFps;
        recvMbps = state_.recvMbps;
        avgJpegKb = state_.avgJpegKb;
        avgPart0Kb = state_.avgPart0Kb;
        avgPart1Kb = state_.avgPart1Kb;
        recvParts = state_.recvParts;
        partStatCount = state_.latestPartStatCount;
        availableEncodeCpuCount = state_.availableEncodeCpuCount;
        for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
            partKb[i] = state_.latestPartKb[i];
            partMs[i] = state_.latestPartMs[i];
            partCpu[i] = state_.latestPartCpu[i];
            partCpuFreqKhz[i] = state_.latestPartCpuFreqKhz[i];
            partLeft[i] = state_.latestPartLeft[i];
            partTop[i] = state_.latestPartTop[i];
            partWidth[i] = state_.latestPartWidth[i];
            partHeight[i] = state_.latestPartHeight[i];
            partSharePermille[i] = state_.latestPartSharePermille[i];
        }
        streamResetGeneration = state_.streamResetGeneration;
        if (state_.hasFrame && state_.latest.generation != uploadedGeneration_) {
            newest = std::move(state_.latest);
            state_.hasFrame = false;
            gotFrame = true;
        }
    }

    if (streamResetGeneration != observedStreamResetGeneration_) {
        observedStreamResetGeneration_ = streamResetGeneration;
        clearDisplayedFrameAfterStreamReset();
    }

    const bool hasVisibleFrame = (((frameSrv_ != nullptr && frameTex_ != nullptr) || (currentGpuFrame_ && currentGpuFrame_->srv)) && currentFrame_.generation > 0);
    if (currentStatus_ != status) {
        currentStatus_ = status;
        titleDirty_ = true;
        hudDirty_ = true;
        dirtyWindow_ = true;
    }
    // 更新统计缓存
    if (cachedRecvFps_ != recvFps || cachedDecodeFps_ != decodeFps || cachedDisplayFps_ != displayFps ||
        cachedRecvMbps_ != recvMbps || cachedAvgJpegKb_ != avgJpegKb || cachedPart0Kb_ != avgPart0Kb || cachedPart1Kb_ != avgPart1Kb || cachedRecvParts_ != recvParts) {
        cachedRecvFps_ = recvFps;
        cachedDecodeFps_ = decodeFps;
        cachedDisplayFps_ = displayFps;
        cachedRecvMbps_ = recvMbps;
        cachedAvgJpegKb_ = avgJpegKb;
        cachedPart0Kb_ = avgPart0Kb;
        cachedPart1Kb_ = avgPart1Kb;
        cachedRecvParts_ = recvParts;
        cachedPartStatCount_ = partStatCount;
        cachedAvailableEncodeCpuCount_ = availableEncodeCpuCount;
        for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
            cachedPartKb_[i] = partKb[i];
            cachedPartMs_[i] = partMs[i];
            cachedPartCpu_[i] = partCpu[i];
            cachedPartCpuFreqKhz_[i] = partCpuFreqKhz[i];
            cachedPartLeft_[i] = partLeft[i];
            cachedPartTop_[i] = partTop[i];
            cachedPartWidth_[i] = partWidth[i];
            cachedPartHeight_[i] = partHeight[i];
            cachedPartSharePermille_[i] = partSharePermille[i];
        }
        titleDirty_ = true;
        hudDirty_ = hasVisibleFrame || gotFrame;
        if (hasVisibleFrame || gotFrame) dirtyWindow_ = true;
    }
    // 强制更新缓存（即使值没变也更新）
    cachedPartStatCount_ = partStatCount;
    cachedAvailableEncodeCpuCount_ = availableEncodeCpuCount;
    for (int i = 0; i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
        cachedPartKb_[i] = partKb[i];
        cachedPartMs_[i] = partMs[i];
        cachedPartCpu_[i] = partCpu[i];
        cachedPartCpuFreqKhz_[i] = partCpuFreqKhz[i];
        cachedPartLeft_[i] = partLeft[i];
        cachedPartTop_[i] = partTop[i];
        cachedPartWidth_[i] = partWidth[i];
        cachedPartHeight_[i] = partHeight[i];
        cachedPartSharePermille_[i] = partSharePermille[i];
    }
    if (gotFrame && newest.generation != 0) {
        cachedCaptureMs_ = newest.captureMs;
        cachedEncodeMs_ = newest.encodeMs;
        cachedQueueMs_ = newest.queueMs;
        cachedSocketMs_ = newest.socketMs;
        cachedDecodeWallMs_ = newest.decodeMs;
        cachedDecodeCpuSumMs_ = newest.decodeCpuSumMs;
        cachedDecodeMaxPartMs_ = newest.decodeMaxPartMs;
        cachedDecodeTailWaitMs_ = newest.decodeTailWaitMs;
        cachedDecodeOverlapSavedMs_ = newest.decodeOverlapSavedMs;
        cachedDecodePartCount_ = newest.decodePartCount > 0 ? newest.decodePartCount : 1;
    }

    if (!gotFrame || newest.generation == 0 || newest.generation == uploadedGeneration_) {
        return false;
    }

    const bool newestIsGpuFrame = newest.gpuFrame && newest.gpuFrame->srv;
    if (!newestIsGpuFrame) {
        currentGpuFrame_.reset();
        if (!ensureFrameTexture(newest.width, newest.height)) return false;
    }

    if (uploadedGeneration_ != 0 && newest.generation > uploadedGeneration_ + 1) {
        skippedFramesWindow_ += int(newest.generation - uploadedGeneration_ - 1);
    }
    currentFrame_ = std::move(newest);
    currentGpuFrame_ = currentFrame_.gpuFrame;
    uploadedGeneration_ = currentFrame_.generation;
    fitWindowToFrameIfNeeded();
    if (currentGpuFrame_ && currentGpuFrame_->srv) {
        lastUploadMode_ = 4;
        lastUploadRowPitch_ = 0;
        lastUploadCpuMs_ = 0.0;
    } else {
        const int64_t uploadBeginNs = NowNs();
        const bool uploadOk = uploadFrameTextureDynamic(currentFrame_);
        const int64_t uploadDoneNs = NowNs();
        lastUploadCpuMs_ = (std::max)(0.0, double(uploadDoneNs - uploadBeginNs) / 1000000.0);
        if (!uploadOk) return false;
    }
    dirtyWindow_ = true;
    titleDirty_ = true;
    hudDirty_ = true;
    return true;
}

// ---------- 顶点更新 ----------
void D3D11Renderer::updateVertices() {
    if (frameVerticesInitialized_) return;
    Vertex v[] = {
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 0.0f, 1.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f,  1.0f, 0.0f, 0.0f, 0.0f },
        {  1.0f, -1.0f, 0.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f, 1.0f },
    };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx_->Map(vertexBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, v, sizeof(v));
        ctx_->Unmap(vertexBuffer_, 0);
        frameVerticesInitialized_ = true;
    }
}




// ---------- 渲染 ----------
void D3D11Renderer::render(bool presentedNewFrame) {
    if (!rtv_) return;
    const int64_t renderBeginNs = NowNs();

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int clientW = rc.right - rc.left;
    int clientH = rc.bottom - rc.top;

    // ========== 调试日志（写入 %TEMP%\debug_log.txt） ==========
    static int dbgCount = 0;
    if (dbgCount++ % 30 == 0) {
        wchar_t tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        wcscat_s(tempPath, L"debug_log.txt");
        FILE* fp = nullptr;
        _wfopen_s(&fp, tempPath, L"a");
        if (fp) {
            fwprintf(fp, L"Client: %dx%d  Frame: %dx%d  Stretch: %d\n",
                     clientW, clientH, currentFrame_.width, currentFrame_.height, stretch_ ? 1 : 0);
            fclose(fp);
        }
    }

    // ========== 强制视口为整个客户区 ==========
    float clear[4] = { 0, 0, 0, 1 };
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>((std::max)(1L, clientW));
    vp.Height = static_cast<float>((std::max)(1L, clientH));
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    // frameVp 直接等于 vp，确保铺满窗口
    D3D11_VIEWPORT frameVp = vp;

    // 即使 stretch_ 为 false，也强制全屏（覆盖原有比例计算）
    // 如果想保留比例，可在此处添加 if (!stretch_) 判断，但暂时强制拉伸
    frameVp = vp;

    updateVertices();

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    ctx_->RSSetViewports(1, &vp);
    ctx_->ClearRenderTargetView(rtv_, clear);

    ctx_->RSSetViewports(1, &frameVp);
    float noBlend[4] = { 0,0,0,0 };
    ctx_->OMSetBlendState(nullptr, noBlend, 0xffffffff);
    ctx_->IASetInputLayout(inputLayout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);

    ID3D11ShaderResourceView* visibleFrameSrv = (currentGpuFrame_ && currentGpuFrame_->srv) ? currentGpuFrame_->srv : frameSrv_;
    if (visibleFrameSrv) {
        ctx_->IASetVertexBuffers(0, 1, &vertexBuffer_, &stride, &offset);
        ctx_->PSSetShaderResources(0, 1, &visibleFrameSrv);
        ctx_->Draw(6, 0);
        ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
        ctx_->PSSetShaderResources(0, 1, nullSrv);
    }

    // HUD
    ctx_->RSSetViewports(1, &vp);
    if (hudVisible_) {
        updateHudTextureIfNeeded(false);
        drawHud(vp);
    }

    const int64_t presentBeginNs = NowNs();
    lastDrawCpuMs_ = (std::max)(0.0, double(presentBeginNs - renderBeginNs) / 1000000.0);
    swapChain_->Present(0, allowTearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
    const int64_t presentDoneNs = NowNs();

    lastPresentCpuMs_ = (std::max)(0.0, double(presentDoneNs - presentBeginNs) / 1000000.0);
    lowerBoundMs_ = currentFrame_.captureMs + currentFrame_.encodeMs + currentFrame_.queueMs +
                    currentFrame_.socketMs + currentFrame_.decodeMs +
                    lastUploadCpuMs_ + lastDrawCpuMs_ + lastPresentCpuMs_;
    lowerBoundEqFps_ = lowerBoundMs_ > 0.0 ? 1000.0 / lowerBoundMs_ : 0.0;

    if (presentedNewFrame) {
        if (lastPresentDoneNs_ > 0) {
            const double ivMs = (std::max)(0.0, double(presentDoneNs - lastPresentDoneNs_) / 1000000.0);
            if (ivMs >= 1.0 && ivMs <= 1000.0) {
                presentIntervalLastMs_ = ivMs;
                presentIntervalAccumMs_ += ivMs;
                presentIntervalSamples_ += 1;
                presentIntervalMaxMs_ = (std::max)(presentIntervalMaxMs_, ivMs);
                presentIntervalsWindow_.push_back(ivMs);
            }
        }
        lastPresentDoneNs_ = presentDoneNs;
        lastRenderBeginNs_ = renderBeginNs;

        presentIntervalShownAvgMs_ = presentIntervalSamples_ > 0 ? (presentIntervalAccumMs_ / presentIntervalSamples_) : presentIntervalLastMs_;
        presentIntervalShownMaxMs_ = (std::max)(presentIntervalMaxMs_, presentIntervalLastMs_);
        skippedFramesLast_ = skippedFramesWindow_;
        if (!presentIntervalsWindow_.empty()) {
            std::vector<double> sorted = presentIntervalsWindow_;
            std::sort(sorted.begin(), sorted.end());
            presentIntervalP95Ms_ = percentileFromSorted(sorted, 95.0);
            presentIntervalP99Ms_ = percentileFromSorted(sorted, 99.0);
        }
    }

    if (presentedNewFrame && currentFrame_.generation > 0 && g_perfLog.isRecording()) {
        char row[1024]{};
        std::snprintf(row, sizeof(row),
            "0,%llu,0,%d,%d,-1,%d,%d,%d,0,0,%d,%d,%d,%d,0,%.3f,%.3f,%.3f,%.3f,0,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,-1,0,0,%d,%u,render_present",
            static_cast<unsigned long long>(currentFrame_.generation),
            currentFrame_.width, currentFrame_.height,
            (std::max)(1, currentFrame_.decodePartCount),
            currentFrame_.width, currentFrame_.height,
            currentFrame_.width, currentFrame_.height,
            currentFrame_.width, currentFrame_.height,
            currentFrame_.captureMs, currentFrame_.encodeMs, currentFrame_.queueMs, currentFrame_.socketMs,
            currentFrame_.decodeMs, currentFrame_.decodeCpuSumMs, currentFrame_.decodeMaxPartMs,
            currentFrame_.decodeTailWaitMs, currentFrame_.decodeOverlapSavedMs,
            lastUploadCpuMs_, lastDrawCpuMs_, lastPresentCpuMs_,
            presentIntervalLastMs_, skippedFramesLast_, cachedRecvMbps_, cachedDisplayFps_,
            lastUploadMode_, static_cast<unsigned int>(lastUploadRowPitch_));
        g_perfLog.recordRow("render_present", row);
    }

    auto statsNow = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(statsNow - presentStatsWindowStart_).count() >= 1.0) {
        uploadMapCountShown_ = uploadMapCountWindow_;
        uploadFallbackCountShown_ = uploadFallbackCountWindow_;
        uploadFailedCountShown_ = uploadFailedCountWindow_;
        uploadMapCountWindow_ = 0;
        uploadFallbackCountWindow_ = 0;
        uploadFailedCountWindow_ = 0;
        presentIntervalAccumMs_ = 0.0;
        presentIntervalSamples_ = 0;
        presentIntervalMaxMs_ = 0.0;
        skippedFramesWindow_ = 0;
        presentIntervalsWindow_.clear();
        presentStatsWindowStart_ = statsNow;
    }

    hudDirty_ = true;
    titleDirty_ = true;
}

// ---------- 全屏切换 ----------
void D3D11Renderer::toggleFullscreen() {
    fullscreen_ = !fullscreen_;
    if (fullscreen_) {
        GetWindowRect(hwnd_, &windowedRect_);
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd_, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        windowSizedToFrame_ = false;
        fitWindowToFrameIfNeeded();
        if (!windowSizedToFrame_) {
            SetWindowPos(hwnd_, nullptr, windowedRect_.left, windowedRect_.top,
                         windowedRect_.right - windowedRect_.left,
                         windowedRect_.bottom - windowedRect_.top,
                         SWP_FRAMECHANGED | SWP_NOZORDER);
        }
    }
    dirtyWindow_ = true;
    titleDirty_ = true;
}

void D3D11Renderer::fitWindowToFrameIfNeeded() {
    if (fullscreen_ || !currentFrame_.width || !currentFrame_.height) return;
    RECT rect{0,0, currentFrame_.width, currentFrame_.height};
    AdjustWindowRect(&rect, GetWindowLongW(hwnd_, GWL_STYLE), FALSE);
    SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    windowSizedToFrame_ = true;
}

// ---------- HUD 更新 ----------
void D3D11Renderer::updateHudTextureIfNeeded(bool force) {
    if (!hudDirty_ && !force) return;
    if (!hudTex_ || !ctx_) return;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = ctx_->Map(hudTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    const int width = hudTexWidth_;
    const int height = hudTexHeight_;

    // 使用 hudPixels_ 作为中间缓冲区
    const size_t pixelCount = static_cast<size_t>(width) * height;
    hudPixels_.assign(pixelCount * 4, 0);

    // 准备文字行（保留中文，但点阵字体只支持 ASCII，中文将显示为 '?'）
    std::vector<std::wstring> lines;
    lines.push_back(L"Status: " + currentStatus_);

    wchar_t buf[256];
    if (cachedRecvFps_ > 0.0 || cachedDecodeFps_ > 0.0 || cachedDisplayFps_ > 0.0) {
        swprintf_s(buf, 256, L"Recv: %.1f fps  %.1f Mbps  Dec: %.1f fps  Disp: %.1f fps",
                   cachedRecvFps_, cachedRecvMbps_, cachedDecodeFps_, cachedDisplayFps_);
        lines.push_back(buf);
    }
    if (cachedAvgJpegKb_ > 0.0) {
        swprintf_s(buf, 256, L"JPEG: %.1f KB  Parts: %d  (0:%.1fKB 1:%.1fKB)",
                   cachedAvgJpegKb_, cachedRecvParts_, cachedPart0Kb_, cachedPart1Kb_);
        lines.push_back(buf);
    }
    // 分块统计（保留英文部分，中文会变 '?'，但数据是数字，无妨）
    appendPartStatsHudLines(lines);

    if (lowerBoundMs_ > 0.0) {
        swprintf_s(buf, 256, L"Latency: %.1f ms  (capture %.1f encode %.1f queue %.1f socket %.1f decode %.1f upload %.1f draw %.1f present %.1f)",
                   lowerBoundMs_, cachedCaptureMs_, cachedEncodeMs_, cachedQueueMs_,
                   cachedSocketMs_, cachedDecodeWallMs_, lastUploadCpuMs_, lastDrawCpuMs_, lastPresentCpuMs_);
        lines.push_back(buf);
    }
    if (presentIntervalShownAvgMs_ > 0.0) {
        swprintf_s(buf, 256, L"Interval: avg %.2f ms  max %.2f ms  p95 %.2f ms  p99 %.2f ms",
                   presentIntervalShownAvgMs_, presentIntervalShownMaxMs_, presentIntervalP95Ms_, presentIntervalP99Ms_);
        lines.push_back(buf);
    }
    if (skippedFramesLast_ > 0) {
        swprintf_s(buf, 256, L"Skipped: %d", skippedFramesLast_);
        lines.push_back(buf);
    }
    if (uploadMapCountShown_ > 0 || uploadFallbackCountShown_ > 0) {
        swprintf_s(buf, 256, L"Upload: Map %d  Fallback %d  Fail %d  mode %d  pitch %u",
                   uploadMapCountShown_, uploadFallbackCountShown_, uploadFailedCountShown_, lastUploadMode_, lastUploadRowPitch_);
        lines.push_back(buf);
    }
    if (cachedDisplayFps_ > 0.0) {
        swprintf_s(buf, 256, L"Equiv FPS: %.1f", lowerBoundEqFps_);
        lines.push_back(buf);
    }
    if (cachedDecodePartCount_ > 1) {
        swprintf_s(buf, 256, L"Decode parts: %d  wall %.1fms  CPU %.1fms  max %.1fms  tail %.1fms  saved %.1fms",
                   cachedDecodePartCount_, cachedDecodeWallMs_, cachedDecodeCpuSumMs_,
                   cachedDecodeMaxPartMs_, cachedDecodeTailWaitMs_, cachedDecodeOverlapSavedMs_);
        lines.push_back(buf);
    }

    // 使用点阵字体绘制每一行
    const int scale = 2;
    int yPos = 10;
    const uint8_t fgB = 255, fgG = 255, fgR = 255, fgA = 255; // 白色

    for (const auto& wline : lines) {
        // 将宽字符串转为窄字符串，点阵字体只支持 ASCII
        std::string narrow;
        for (wchar_t ch : wline) {
            if (ch >= 32 && ch <= 126) {
                narrow.push_back(static_cast<char>(ch));
            } else {
                // 非 ASCII 用 '?' 代替
                narrow.push_back('?');
            }
        }
        if (narrow.empty()) continue;
        // 使用 HudDrawText 绘制到 hudPixels_
        HudDrawText(hudPixels_, width, height, 10, yPos, narrow.c_str(), scale, fgB, fgG, fgR, fgA);
        yPos += (7 + 1) * scale; // 每行高度
        if (yPos >= height) break;
    }

    // 将 hudPixels_ 复制到纹理
    uint8_t* dst = static_cast<uint8_t*>(mapped.pData);
    std::memcpy(dst, hudPixels_.data(), hudPixels_.size());

    ctx_->Unmap(hudTex_, 0);
    hudDirty_ = false;
}

void D3D11Renderer::appendPartStatsHudLines(std::vector<std::wstring>& lines) const {
    if (cachedPartStatCount_ <= 0) return;
    wchar_t buf[512];
    for (int i = 0; i < cachedPartStatCount_ && i < MAX_RUNTIME_SPLIT_PARTS; ++i) {
        swprintf_s(buf, 512, L"  Part %d: L=%d T=%d %dx%d  %.1fKB  cpu%d freq%d  enc%.1fms  share%d%%",
                   i, cachedPartLeft_[i], cachedPartTop_[i], cachedPartWidth_[i], cachedPartHeight_[i],
                   cachedPartKb_[i], cachedPartCpu_[i], cachedPartCpuFreqKhz_[i],
                   cachedPartMs_[i], cachedPartSharePermille_[i]);
        lines.push_back(buf);
    }
}

void D3D11Renderer::drawHud(const D3D11_VIEWPORT& vp) {
    if (!hudSrv_ || !hudVertexBuffer_) return;

    float blendFactor[4] = { 0,0,0,0 };
    ctx_->OMSetBlendState(alphaBlend_, blendFactor, 0xffffffff);
    ctx_->IASetInputLayout(inputLayout_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ctx_->IASetVertexBuffers(0, 1, &hudVertexBuffer_, &stride, &offset);
    ctx_->PSSetShaderResources(0, 1, &hudSrv_);
    ctx_->Draw(6, 0);

    // 恢复 blend state
    float noBlend[4] = {0,0,0,0};
    ctx_->OMSetBlendState(nullptr, noBlend, 0xffffffff);
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx_->PSSetShaderResources(0, 1, nullSrv);
}

// ---------- 杂项 ----------
void D3D11Renderer::drawSplitDebugOverlay(uint8_t* bgra, int width, int height, int pitch) {
    // 可选：绘制分块边框，目前留空
}

double D3D11Renderer::totalHudKb() const {
    // 仅主帧压缩大小，若无可返回0
    return cachedAvgJpegKb_ > 0.0 ? cachedAvgJpegKb_ : 0.0;
}

double D3D11Renderer::totalHudMbps() const {
    return cachedRecvMbps_;
}

// ---------- 消息处理 ----------
LRESULT CALLBACK D3D11Renderer::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    D3D11Renderer* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = static_cast<D3D11Renderer*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
        return DefWindowProc(hwnd, msg, wp, lp);
    }
    self = reinterpret_cast<D3D11Renderer*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProc(hwnd, msg, wp, lp);
    return self->handleMessage(msg, wp, lp);
}

LRESULT D3D11Renderer::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        state_.stop.store(true);
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            state_.stop.store(true);
            DestroyWindow(hwnd_);
            return 0;
        }
        if (wp == VK_F11 || wp == VK_RETURN) {
            toggleFullscreen();
            return 0;
        }
        if (wp == VK_F3) {
            hudVisible_ = !hudVisible_;
            dirtyWindow_ = true;
            return 0;
        }
        break;
    case WM_SIZE:
        if (swapChain_ && wp != SIZE_MINIMIZED) {
            // 释放旧的 RTV
            if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
            // 获取新客户区大小
            RECT rc;
            GetClientRect(hwnd_, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                // 调整交换链缓冲大小，匹配窗口尺寸
                HRESULT hr = swapChain_->ResizeBuffers(2, w, h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        allowTearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                if (SUCCEEDED(hr)) {
                    recreateRTV();
                }
            }
            dirtyWindow_ = true;
        }
        break;
    case WM_PAINT:
        dirtyWindow_ = true;
        break;
    }
    return DefWindowProc(hwnd_, msg, wp, lp);
}

// ---------- 主循环 ----------
int D3D11Renderer::run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);

    MSG msg{};
    displayWindowStart_ = std::chrono::steady_clock::now();
    int displayCount = 0;
    int idleSpin = 0;

    while (!state_.stop.load()) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                state_.stop.store(true);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (state_.stop.load()) break;

        const bool hadNewFrame = consumeLatestFrame();
        if (hadNewFrame || dirtyWindow_) {
            idleSpin = 0;
            render(hadNewFrame);
            dirtyWindow_ = false;
            if (hadNewFrame) {
                displayCount++;
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration<double>(now - displayWindowStart_).count();
                if (elapsed >= 1.0) {
                    std::lock_guard<std::mutex> lk(state_.mutex);
                    state_.displayFps = displayCount / elapsed;
                    cachedDisplayFps_ = state_.displayFps;
                    displayCount = 0;
                    displayWindowStart_ = now;
                    titleDirty_ = true;
                    hudDirty_ = true;
                }
            }
            maybeUpdateWindowTitle(false);
            continue;
        }
        maybeUpdateWindowTitle(false);

        if (idleSpin < 2) {
            ++idleSpin;
            Sleep(0);
        } else {
            idleSpin = 0;
            HANDLE waitHandles[1];
            DWORD handleCount = 0;
            if (state_.frameReadyEvent) {
                waitHandles[handleCount++] = state_.frameReadyEvent;
            }
            MsgWaitForMultipleObjectsEx(handleCount, waitHandles, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
    }
    timeEndPeriod(1);
    return 0;
}

void D3D11Renderer::maybeUpdateWindowTitle(bool force) {
    // 本示例不更新标题，保留为空
}

// ---------- 清理 ----------
void D3D11Renderer::cleanup() {
    if (ctx_) ctx_->ClearState();
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (swapChain2_) { swapChain2_->Release(); swapChain2_ = nullptr; }
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (ps_) { ps_->Release(); ps_ = nullptr; }

    if (alphaBlend_) { alphaBlend_->Release(); alphaBlend_ = nullptr; }
    if (frameTex_) { frameTex_->Release(); frameTex_ = nullptr; }
    if (frameSrv_) { frameSrv_->Release(); frameSrv_ = nullptr; }
    if (hudTex_) { hudTex_->Release(); hudTex_ = nullptr; }
    if (hudSrv_) { hudSrv_->Release(); hudSrv_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}
