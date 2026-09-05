#include <windows.h>
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "studio.h"
#include "relative_time.h"
#include <d3d11.h>
#include <fstream>
#include <shellapi.h>
#include <vector>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
static ComPtr<ID3D11Device> device;
static ComPtr<ID3D11DeviceContext> deviceContext;
static ComPtr<IDXGISwapChain> swapchain;
static ComPtr<ID3D11RenderTargetView> target;
static UINT resizeWidth = 0, resizeHeight = 0;
static bool MakeTarget() {
    ComPtr<ID3D11Texture2D> back;
    return SUCCEEDED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))) &&
           SUCCEEDED(device->CreateRenderTargetView(back.Get(), nullptr, &target));
}
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT WINAPI WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp)
            return 0;
        break;
    case WM_NCHITTEST: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &p);
        RECT r;
        GetClientRect(hwnd, &r);
        bool l = p.x<5, rr = p.x> r.right - 5, t = p.y<5, b = p.y> r.bottom - 5;
        if (t && l)
            return HTTOPLEFT;
        if (t && rr)
            return HTTOPRIGHT;
        if (b && l)
            return HTBOTTOMLEFT;
        if (b && rr)
            return HTBOTTOMRIGHT;
        if (l)
            return HTLEFT;
        if (rr)
            return HTRIGHT;
        if (t)
            return HTTOP;
        if (b)
            return HTBOTTOM;
        if (p.y < 19 && p.x > 28 && p.x < r.right - 30)
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            resizeWidth = LOWORD(lp);
            resizeHeight = HIWORD(lp);
        }
        return 0;
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lp);
        info->ptMinTrackSize = {540, 500};
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_DPICHANGED: {
        auto *rect = reinterpret_cast<RECT *>(lp);
        SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
static bool Capture(const std::filesystem::path &path) {
    ComPtr<ID3D11Texture2D> back, readback;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&back))))
        return false;
    D3D11_TEXTURE2D_DESC desc{};
    back->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &readback)))
        return false;
    deviceContext->CopyResource(readback.Get(), back.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(deviceContext->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT hr =
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr))
        hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr))
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr))
        hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr))
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr))
        hr = encoder->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr))
        hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr))
        hr = frame->SetSize(desc.Width, desc.Height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr))
        hr = frame->SetPixelFormat(&format);
    std::vector<BYTE> pixels(desc.Width * desc.Height * 4);
    for (UINT y = 0; y < desc.Height; y++)
        for (UINT x = 0; x < desc.Width; x++) {
            auto *src = static_cast<BYTE *>(mapped.pData) + y * mapped.RowPitch + x * 4;
            auto *dst = pixels.data() + (y * desc.Width + x) * 4;
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    if (SUCCEEDED(hr))
        hr = frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    if (SUCCEEDED(hr))
        hr = frame->Commit();
    if (SUCCEEDED(hr))
        hr = encoder->Commit();
    deviceContext->Unmap(readback.Get(), 0);
    return SUCCEEDED(hr);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    wchar_t exePath[32768];
    GetModuleFileNameW(nullptr, exePath, 32768);
    auto root = std::filesystem::path(exePath).parent_path();
    int argc;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int theme = -1, tab = -1, width = 1000, height = 560, font = -1;
    float scale = -1;
    std::filesystem::path capture;
    std::string captureState;
    bool selfTest = false, probe = false, fakeServer = false, switchTest = false;
    std::string switchId;
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--self-test")
            selfTest = true;
        else if (arg == L"--fake-server")
            fakeServer = true;
        else if (arg == L"--switch-helper" && i + 1 < argc)
            switchId = accounts::Utf8(argv[++i]);
        else if (arg == L"--switch-helper-test" && i + 1 < argc) {
            switchId = accounts::Utf8(argv[++i]);
            switchTest = true;
        } else if (arg == L"--probe")
            probe = true;
        else if (arg == L"--capture" && i + 1 < argc)
            capture = argv[++i];
        else if (arg == L"--capture-state" && i + 1 < argc)
            captureState = accounts::Utf8(argv[++i]);
        else if (arg == L"--theme" && i + 1 < argc)
            theme = _wtoi(argv[++i]);
        else if (arg == L"--tab" && i + 1 < argc)
            tab = _wtoi(argv[++i]);
        else if (arg == L"--width" && i + 1 < argc)
            width = _wtoi(argv[++i]);
        else if (arg == L"--height" && i + 1 < argc)
            height = _wtoi(argv[++i]);
        else if (arg == L"--font" && i + 1 < argc)
            font = _wtoi(argv[++i]);
        else if (arg == L"--scale" && i + 1 < argc)
            scale = static_cast<float>(_wtof(argv[++i]));
    }
    LocalFree(argv);
    if (fakeServer)
        return accounts::FakeServer();
    if (!switchId.empty())
        return accounts::SwitchHelper(switchId, switchTest);
    if (selfTest)
        return SettingsSelfTest() && accounts::SelfTest() && accounts::ProtocolSelfTest() &&
                       accounts::SwitchSelfTest() && accounts::SwitchHelperSelfTest() &&
                       RelativeTimeSelfTest() && Studio::UiSelfTest()
                   ? 0
                   : 1;
    if (probe) {
        std::atomic_bool cancel = false;
        try {
            return accounts::Probe(cancel) ? 0 : 1;
        } catch (...) {
            return 2;
        }
    }
    HANDLE instanceLock = nullptr;
    if (capture.empty()) {
        instanceLock = CreateMutexW(nullptr, FALSE, L"Local\\CodexSwitcher.ImGui");
        if (!instanceLock || WaitForSingleObject(instanceLock, 0) != WAIT_OBJECT_0) {
            MessageBoxW(nullptr, L"Codex Switcher is already open.", L"Codex Switcher", MB_OK);
            if (instanceLock)
                CloseHandle(instanceLock);
            return 0;
        }
    }
    ImGui_ImplWin32_EnableDpiAwareness();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WNDCLASSEXW wc = {sizeof(wc),
                      CS_CLASSDC,
                      WindowProc,
                      0,
                      0,
                      instance,
                      nullptr,
                      LoadCursor(nullptr, IDC_ARROW),
                      nullptr,
                      nullptr,
                      L"CodexSwitcherWindow",
                      nullptr};
    RegisterClassExW(&wc);
    RECT rect = {0, 0, std::max(width, 520), std::max(height, 460)};
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Codex Switcher - Dear ImGui",
                              WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX, 180, 100,
                              rect.right, rect.bottom, nullptr, nullptr, instance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr =
        D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                      D3D11_SDK_VERSION, &sd, &swapchain, &device, &level, &deviceContext);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &swapchain, &device, &level,
                                           &deviceContext);
    if (FAILED(hr) || !MakeTarget()) {
        MessageBoxW(hwnd, L"DirectX 11 could not initialize.", L"ImGui Studio", MB_ICONERROR);
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    Studio studio(root, !capture.empty());
    studio.InitFonts();
    if (theme >= 0 && theme < 7)
        studio.settings.theme = theme;
    if (font >= 0 && font < 3)
        studio.settings.font = font;
    if (scale >= .85f && scale <= 1.6f)
        studio.settings.scale = scale;
    studio.forcedTab = tab;
    studio.captureState = captureState;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device.Get(), deviceContext.Get());
    if (capture.empty())
        ShowWindow(hwnd, SW_SHOWDEFAULT);
    bool done = false, occluded = false;
    int frames = 0, exitCode = 0;
    while (!done && !studio.exitRequested) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        studio.Tick();
        if (capture.empty() && (IsIconic(hwnd) || (occluded && swapchain->Present(0, DXGI_PRESENT_TEST) ==
                                                                   DXGI_STATUS_OCCLUDED))) {
            Sleep(20);
            continue;
        }
        if (resizeWidth && resizeHeight) {
            target.Reset();
            hr = swapchain->ResizeBuffers(0, resizeWidth, resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            resizeWidth = resizeHeight = 0;
            if (FAILED(hr) || !MakeTarget()) {
                exitCode = 2;
                break;
            }
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        studio.Draw();
        ImGui::Render();
        const float clear[] = {.08f, .08f, .09f, 1};
        auto *rtv = target.Get();
        deviceContext->OMSetRenderTargets(1, &rtv, nullptr);
        deviceContext->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!capture.empty() && ++frames == 6) {
            std::filesystem::create_directories(capture.parent_path());
            exitCode = Capture(capture) ? 0 : 3;
            break;
        }
        hr = swapchain->Present(1, 0);
        occluded = hr == DXGI_STATUS_OCCLUDED;
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            exitCode = 2;
            break;
        }
    }
    if (capture.empty())
        studio.Save();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    target.Reset();
    swapchain.Reset();
    deviceContext.Reset();
    device.Reset();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    CoUninitialize();
    if (instanceLock) {
        ReleaseMutex(instanceLock);
        CloseHandle(instanceLock);
    }
    return exitCode;
}
