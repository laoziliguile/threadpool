// app/main.cpp —— 桌面程序的入口：建窗口、建 D3D11 设备、跑 ImGui 主循环
//
// 这是整个 GUI 里唯一跟 Windows API 打交道的"脏活"文件。
// 建议的阅读顺序：
//   1. wWinMain       —— 程序入口，注册窗口类、建窗口、初始化、进主循环
//   2. create_device  —— 创建 D3D11 设备 + 交换链
//   3. wnd_proc       —— 窗口消息回调（大小变化、关闭）
//   4. 主循环          —— NewFrame → 画界面 → Render → Present
//
// 记住一条主线：ImGui 自己不会开窗口，它只是在"别人的"绘制上下文上画三角形。
// 所以我们先用 Win32 造一个窗口，再用 D3D11 造一个渲染目标，最后把 ImGui 挂上去。
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include <d3d11.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "app/dashboard.hpp"
#include "app/theme.hpp"
#include "threadpool/pool.hpp"

// ImGui 有意没有在头文件里声明这个函数（否则每个包含头文件的地方都得拖进 <windows.h>），
// 它要求使用方自己写一行前置声明。照抄即可。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wparam,
                                                             LPARAM lparam);

namespace {

// ---- D3D11 的四个全局对象：设备、上下文、交换链、渲染目标视图 ----
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap_chain = nullptr;
ID3D11RenderTargetView* g_render_target = nullptr;

bool g_running = true;         // 主循环开关（收到 WM_CLOSE 就置 false）
UINT g_pending_width = 0;      // 窗口刚被拖动过：等待重建的大小（0 表示不需要重建）
UINT g_pending_height = 0;

// ---------------------------------------------------------------------------
// 创建渲染目标：交换链里的后备缓冲没法直接画，要先包一层 RTV
// ---------------------------------------------------------------------------
bool create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    const HRESULT hr = g_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr) || !back_buffer) return false;

    const HRESULT view_hr =
        g_device->CreateRenderTargetView(back_buffer, nullptr, &g_render_target);
    back_buffer->Release();
    // 失败时 g_render_target 仍是 nullptr：调用方必须据此放弃这一帧，
    // 否则就是对空 COM 指针调 ClearRenderTargetView。
    return SUCCEEDED(view_hr) && g_render_target != nullptr;
}

void cleanup_render_target() {
    if (g_render_target) {
        g_render_target->Release();
        g_render_target = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 创建 D3D11 设备与交换链。硬件不行就退回 WARP（软件光栅化器），保证一定能跑。
// ---------------------------------------------------------------------------
bool create_device(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;   // 0 = 跟随窗口大小
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // 8 位 RGBA，最通用的格式
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;   // 不做多重采样（ImGui 自己会抗锯齿）
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_11_0;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swap_chain, &g_device, &feature_level, &g_context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap_chain, &g_device,
                                           &feature_level, &g_context);
    }
    if (FAILED(hr) || !g_device || !g_swap_chain || !g_context) {
        std::fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed: 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    return create_render_target();
}

void destroy_device() {
    cleanup_render_target();
    if (g_swap_chain) {
        g_swap_chain->Release();
        g_swap_chain = nullptr;
    }
    if (g_context) {
        g_context->Release();
        g_context = nullptr;
    }
    if (g_device) {
        g_device->Release();
        g_device = nullptr;
    }
}

// ---------------------------------------------------------------------------
// 窗口消息回调。
// 注意：所有消息都要先交给 ImGui（它要知道鼠标和键盘），剩下的我们再处理。
// ---------------------------------------------------------------------------
LRESULT WINAPI wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return true;

    switch (msg) {
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED) {
                g_pending_width = static_cast<UINT>(LOWORD(lparam));
                g_pending_height = static_cast<UINT>(HIWORD(lparam));
            }
            return 0;
        case WM_SYSCOMMAND:
            // 屏蔽 Alt 键唤出菜单栏的行为，否则按 Alt 会卡住输入
            if ((wparam & 0xFFF0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            ::DestroyWindow(hwnd);  // 触发 WM_DESTROY
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            g_running = false;
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// 让界面随系统缩放（100% → 1.0，125% → 1.25），否则高分屏上字会很小
float system_ui_scale(HWND hwnd) {
    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (scale < 0.75f || scale > 3.0f) scale = 1.0f;
    return scale;
}

}  // namespace

// ===========================================================================
//  wWinMain：程序入口（-municode 让 MinGW 用宽字符版本的入口函数）
// ===========================================================================
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE /*prev_instance*/, LPWSTR cmd_line,
                      int /*show_cmd*/) {
    const bool demo_mode = (cmd_line != nullptr) && (std::wcsstr(cmd_line, L"--demo") != nullptr);
    ImGui_ImplWin32_EnableDpiAwareness();  // 告诉系统"我自己处理缩放"，界面才清晰

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"ThreadForgeWindowClass";
    if (!::RegisterClassExW(&wc)) return 1;

    // 窗口大小：尽量铺满屏幕，但留出边距；再按窗口样式换算成"外框尺寸"
    const int screen_w = ::GetSystemMetrics(SM_CXSCREEN);
    const int screen_h = ::GetSystemMetrics(SM_CYSCREEN);
    RECT rect = {0, 0, std::max(1280, screen_w - 80), std::max(820, screen_h - 120)};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    ::AdjustWindowRectEx(&rect, style, FALSE, 0);
    const int win_w = rect.right - rect.left;
    const int win_h = rect.bottom - rect.top;
    const int pos_x = (screen_w - win_w) / 2;
    const int pos_y = std::max(0, (screen_h - win_h) / 2);

    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"ThreadForge — 进阶线程池可视化",
                                  style, pos_x, pos_y, win_w, win_h, nullptr, nullptr, instance,
                                  nullptr);
    if (!hwnd) {
        ::UnregisterClassW(wc.lpszClassName, instance);
        return 1;
    }

    if (!create_device(hwnd)) {
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(wc.lpszClassName, instance);
        ::MessageBoxW(nullptr, L"创建 D3D11 设备失败，程序无法启动。", L"ThreadForge", MB_ICONERROR);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // ---- ImGui 初始化 ----
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    const float ui_scale = system_ui_scale(hwnd);  // 窗口建好后才问得到它所在显示器的缩放比
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // 不写 imgui.ini，避免往仓库里扔配置文件
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    app::apply_theme(ui_scale);        // 先定样式（字体大小也按缩放算）
    app::load_fonts(ui_scale);         // 再烤字体图集：必须在 ImGui_ImplDX11_Init 之前
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // ---- 线程池与仪表盘 ----
    tp::ThreadPool pool;
    app::Dashboard dashboard;
    if (demo_mode) {
        pool.start();            // 演示模式：直接开跑，界面上一眼就能看到线程在干活
        dashboard.enable_demo(); // 打开持续压测
    }

    // ---- 主循环：每帧 = 处理消息 + 抓快照画界面 + 提交 GPU ----
    while (g_running) {
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        // 窗口最小化时没必要空转烧 CPU
        if (::IsIconic(hwnd)) {
            ::Sleep(10);
            continue;
        }

        // 窗口大小变了：先释放旧的渲染目标，再让交换链按新尺寸重建缓冲
        if (g_pending_width != 0 && g_pending_height != 0) {
            cleanup_render_target();
            const HRESULT resize_hr = g_swap_chain->ResizeBuffers(
                0, g_pending_width, g_pending_height, DXGI_FORMAT_UNKNOWN, 0);
            g_pending_width = 0;
            g_pending_height = 0;
            if (FAILED(resize_hr) || !create_render_target()) {
                // 缩放失败（设备丢失、尺寸退化成 0 等）：重建不出渲染目标就没法继续画，
                // 直接跳出主循环走正常收尾，而不是拿 nullptr 去 Clear/Present。
                std::fprintf(stderr, "ResizeBuffers/create_render_target failed: 0x%08lX\n",
                             static_cast<unsigned long>(resize_hr));
                break;
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        dashboard.draw(pool);  // 整个界面就在这一行里

        ImGui::Render();
        const ImVec4 bg = app::palette().window_bg;
        const float clear_color[4] = {bg.x, bg.y, bg.z, 1.0f};
        if (!g_render_target) break;  // 兜底：没有渲染目标就不提交这一帧
        g_context->OMSetRenderTargets(1, &g_render_target, nullptr);
        g_context->ClearRenderTargetView(g_render_target, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap_chain->Present(1, 0);  // 1 = 垂直同步，避免撕裂也省 GPU
    }

    // ---- 收尾：顺序和初始化相反 ----
    pool.shutdown(true);  // 优雅关闭：把队列里剩下的任务跑完
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    destroy_device();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, instance);
    return 0;
}
