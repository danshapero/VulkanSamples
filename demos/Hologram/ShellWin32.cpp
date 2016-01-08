#include <cassert>
#include <iostream>
#include <sstream>

#include "Helpers.h"
#include "Game.h"
#include "ShellWin32.h"

ShellWin32::ShellWin32(Game &game) : Shell(game), hwnd_(nullptr)
{
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER *>(&perf_counter_freq_));

    init_window();

    global_extensions_.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
    init_vk();

    game_.attach_shell(*this);
}

ShellWin32::~ShellWin32()
{
    game_.detach_shell();

    cleanup_vk();
    FreeLibrary(hmodule_);

    DestroyWindow(hwnd_);
}

void ShellWin32::init_window()
{
    const std::string class_name(settings_.name + "WindowClass");

    hinstance_ = GetModuleHandle(nullptr);

    WNDCLASSEX win_class = {};
    win_class.cbSize = sizeof(WNDCLASSEX);
    win_class.style = CS_HREDRAW | CS_VREDRAW;
    win_class.lpfnWndProc = window_proc;
    win_class.hInstance = hinstance_;
    win_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    win_class.lpszClassName = class_name.c_str();
    RegisterClassEx(&win_class);

    const DWORD win_style =
        WS_CLIPSIBLINGS | WS_CLIPCHILDREN | WS_VISIBLE | WS_OVERLAPPEDWINDOW;

    RECT win_rect = { 0, 0, settings_.initial_width, settings_.initial_height };
    AdjustWindowRect(&win_rect, win_style, false);

    hwnd_ = CreateWindowEx(WS_EX_APPWINDOW,
                           class_name.c_str(),
                           settings_.name.c_str(),
                           win_style,
                           0,
                           0,
                           win_rect.right - win_rect.left,
                           win_rect.bottom - win_rect.top,
                           nullptr,
                           nullptr,
                           hinstance_,
                           nullptr);

    SetForegroundWindow(hwnd_);
    SetWindowLongPtr(hwnd_, GWLP_USERDATA, (LONG_PTR) this);
}

PFN_vkGetInstanceProcAddr ShellWin32::load_vk()
{
    const char filename[] = "vulkan-0.dll";
    HMODULE mod;
    PFN_vkGetInstanceProcAddr get_proc;

    mod = LoadLibrary(filename);
    if (mod) {
        get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(
                    mod, "vkGetInstanceProcAddr"));
    }

    if (!mod || !get_proc) {
        std::stringstream ss;
        ss << "failed to load or invalid " VULKAN_LOADER;

        if (mod)
            FreeLibrary(mod);

        throw std::runtime_error(ss.str());
    }

    hmodule_ = mod;

    return get_proc;
}

VkSurfaceKHR ShellWin32::create_surface(VkInstance instance)
{
    VkSurfaceKHR surface;
    vk::assert_success(vk::CreateWin32SurfaceKHR(instance, hinstance_, hwnd_, nullptr, &surface));
    return surface;
}

LRESULT ShellWin32::handle_message(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        {
            UINT w = LOWORD(lparam);
            UINT h = HIWORD(lparam);
            resize_swapchain(w, h);
        }
        return 0;
    case WM_KEYUP:
        switch (wparam) {
        case VK_ESCAPE:
            SendMessage(hwnd_, WM_CLOSE, 0, 0);
            break;
        default:
            break;
        }
        return 0;
    default:
        return DefWindowProc(hwnd_, msg, wparam, lparam);
    }
}

float ShellWin32::get_time()
{
    UINT64 count;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER *>(&count));

    return (float) count / perf_counter_freq_;
}

void ShellWin32::run()
{
    resize_swapchain(settings_.initial_width, settings_.initial_height);

    float game_time_base = get_time();

    while (true) {
        bool quit = false;

        assert(settings_.animate);

        // process all messages
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (quit)
            break;

        present(get_time() - game_time_base);
    }

    game_.detach_swapchain();
}
