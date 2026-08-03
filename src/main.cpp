#include <windows.h>
#include <winsock2.h>
#include "mirror_types.h"
#include "mirror_receiver.h"
#include "d3d11_renderer.h"
#include "perf_logger.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(nullptr, L"WSAStartup failed", L"Screen Mirror", MB_ICONERROR);
        return 1;
    }

    SharedState state;
    state.frameReadyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    MirrorReceiver receiver(state, PORT);
    receiver.start();

    D3D11Renderer app(hInstance, state);
    if (!app.init()) {
        state.stop.store(true);
        receiver.stop();
        WSACleanup();
        MessageBoxW(nullptr, L"D3D11 initialization failed", L"Screen Mirror", MB_ICONERROR);
        return 1;
    }

    int rc = app.run();
    std::wstring pendingLogPath;
    g_perfLog.stop(pendingLogPath);
    g_perfLog.stop(pendingLogPath);
    state.stop.store(true);
    receiver.stop();

    if (state.frameReadyEvent) {
        CloseHandle(state.frameReadyEvent);
        state.frameReadyEvent = nullptr;
    }
    WSACleanup();
    return rc;
}