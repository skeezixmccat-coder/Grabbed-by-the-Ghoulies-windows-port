// Opt-in Windows scan-out presentation. The NV2A renderer remains hidden;
// this window displays completed guest scan-out or its scaled GL surface. No D3D HLE.
// GH_PRESENT=1 is interactive; GH_PRESENT=hidden exercises the same message
// and GDI path without showing a window. The first show requires non-silent
// authentic XAudio2 output and a completed scan-out frame.
bool g_present_enabled = false, g_present_hidden = false, g_keyboard_enabled = false;
std::atomic<bool> g_authentic_audio_active{false};
extern "C" volatile long g_draw_dump_request;
std::atomic<HWND> g_present_window{nullptr};
CRITICAL_SECTION g_present_lock;
std::vector<uint32_t> g_present_pixels;
unsigned g_present_width = 640, g_present_height = 480;
uint64_t g_present_serial = 0;
std::atomic<bool> g_present_ready{false};
HANDLE g_present_started = nullptr;
ULONGLONG g_present_start_tick = 0;

bool PresentationHasFocus() {
    HWND window = g_present_window.load();
    return window && GetForegroundWindow() == window;
}

RECT PresentationRect(int width, int height) {
    int w = width, h = width * 3 / 4;
    if (h > height) { h = height; w = height * 4 / 3; }
    return {(width-w)/2, (height-h)/2, (width+w)/2, (height+h)/2};
}

void PaintPresentation(HDC dc, int width, int height) {
    if (width <= 0 || height <= 0) return;
    RECT r = PresentationRect(width,height);
    // Paint only the letterbox bars black: filling the whole client area
    // before the frame is stretched over it showed as a black flash.
    HBRUSH black=static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RECT bars[4]={{0,0,width,r.top},{0,r.bottom,width,height},{0,r.top,r.left,r.bottom},{r.right,r.top,width,r.bottom}};
    for (const RECT& bar : bars) if (bar.right>bar.left && bar.bottom>bar.top) FillRect(dc,&bar,black);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);

    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    EnterCriticalSection(&g_present_lock);
    info.bmiHeader.biWidth=g_present_width; info.bmiHeader.biHeight=-static_cast<LONG>(g_present_height);
    if (!g_present_serial) FillRect(dc,&r,black);
    else {
        SetStretchBltMode(dc,COLORONCOLOR);
        int copied=StretchDIBits(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,0,0,g_present_width,g_present_height,
            g_present_pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
        if (copied==GDI_ERROR || copied==0) Stop(291,"scan-out presentation failed (%lu)",GetLastError());
    }
    LeaveCriticalSection(&g_present_lock);
}

// Borderless fullscreen on the window's monitor (F11 or Alt+Enter toggles;
// Esc in fullscreen returns to the window, Esc in a window closes the game).
static bool g_fullscreen=false; static WINDOWPLACEMENT g_windowed_place{sizeof(WINDOWPLACEMENT)}; static LONG_PTR g_windowed_style=0;
static void ToggleFullscreen(HWND window) {
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (!GetWindowPlacement(window,&g_windowed_place) || !GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi)) return;
        g_windowed_style=GetWindowLongPtrW(window,GWL_STYLE);
        SetWindowLongPtrW(window,GWL_STYLE,(g_windowed_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(window,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED|SWP_SHOWWINDOW);
        g_fullscreen=true;
    } else {
        SetWindowLongPtrW(window,GWL_STYLE,g_windowed_style);
        SetWindowPlacement(window,&g_windowed_place);
        SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED|SWP_SHOWWINDOW);
        g_fullscreen=false;
    }
    InvalidateRect(window,nullptr,TRUE);
}

LRESULT CALLBACK PresentationProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    bool first_press = !(lp & (1<<30));
    if ((message==WM_KEYDOWN && wp==VK_F11 && first_press) || (message==WM_SYSKEYDOWN && wp==VK_RETURN && (lp & (1<<29)) && first_press)) { ToggleFullscreen(window); return 0; }
    if (message==WM_SYSCHAR && wp==VK_RETURN) return 0;   // no "ding" for Alt+Enter
    if (message==WM_KEYDOWN && wp==VK_ESCAPE && g_fullscreen) { if (first_press) ToggleFullscreen(window); return 0; }
    if (message==WM_CLOSE || (message==WM_KEYDOWN && wp==VK_ESCAPE)) Stop(0,"presentation closed by user");
    if (message==WM_KEYDOWN && wp==VK_F9 && !(lp & (1<<30))) { InterlockedExchange(&g_draw_dump_request,1); MessageBeep(MB_OK); return 0; }   // diagnostic draw dump (nv2a_gl)
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_PAINT || message==WM_PRINTCLIENT) {
        PAINTSTRUCT paint{};
        HDC dc=message==WM_PAINT ? BeginPaint(window,&paint) : reinterpret_cast<HDC>(wp);
        RECT r{}; GetClientRect(window,&r); PaintPresentation(dc,r.right,r.bottom);
        if (message==WM_PAINT) EndPaint(window,&paint);
        return 0;
    }
    if (message==WM_TIMER) {
        EnterCriticalSection(&g_present_lock); bool have_frame=g_present_serial!=0; LeaveCriticalSection(&g_present_lock);
        if (!g_present_ready && have_frame && g_authentic_audio_active.load()) {
            if (!g_present_hidden) {
                // Start-Process -WindowStyle Hidden supplies STARTUPINFO/SW_HIDE.
                // Windows overrides the first ShowWindow call with that value.
                // SetWindowPos explicitly reveals only this gated scan-out window,
                // leaving the console and WGL context hidden (MSDN ShowWindow).
                if (!SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW))
                    Stop(291,"cannot show presentation window (%lu)",GetLastError());
                if (!IsWindowVisible(window)) Stop(291,"presentation window remained hidden after show request");
                SetForegroundWindow(window);
            }
            g_present_ready=true;
            Log("presentation: authentic audio and scan-out ready (%s)",g_present_hidden ? "hidden test" : "visible window");
        }
        // Fail visibly through the launcher if startup never reaches the
        // first-view gate. This detects the known loading stall; it does not
        // claim to repair it or silently restart the title.
        if (!g_present_ready && GetTickCount64()-g_present_start_tick >= 45000)
            Stop(292,"preview startup stalled: picture and authentic audio not ready within 45 seconds; close this message and relaunch");
        if (g_present_ready) InvalidateRect(window,nullptr,FALSE);
        return 0;
    }
    return DefWindowProcW(window,message,wp,lp);
}

DWORD WINAPI PresentationThread(void*) {
    // Use the DLL's real PE image; the launcher image is occupied by the XBE.
    HINSTANCE instance=reinterpret_cast<HINSTANCE>(&__ImageBase);
    WNDCLASSW wc{}; wc.lpfnWndProc=PresentationProc; wc.hInstance=instance;
    wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512)); wc.lpszClassName=L"GhouliesNativePresentation";
    if (!RegisterClassW(&wc)) Stop(291,"cannot register presentation window (%lu)",GetLastError());
    RECT work{}; if(!SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)) Stop(291,"cannot read desktop work area");
    RECT r{0,0,960,720}; AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);
    int width=std::min<int>(r.right-r.left,work.right-work.left);
    int height=std::min<int>(r.bottom-r.top,work.bottom-work.top);
    HWND window=CreateWindowExW(0,wc.lpszClassName,L"Grabbed by the Ghoulies \u2014 Native Preview",WS_OVERLAPPEDWINDOW,
        work.left+(work.right-work.left-width)/2,work.top+(work.bottom-work.top-height)/2,width,height,
        nullptr,nullptr,instance,nullptr);
    if(!window) Stop(291,"cannot create presentation window (%lu)",GetLastError());
    g_present_window.store(window);
    if(!SetTimer(window,1,33,nullptr)) Stop(291,"cannot start presentation timer");
    SetEvent(g_present_started);
    MSG message{}; BOOL result;
    while((result=GetMessageW(&message,nullptr,0,0))>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    Stop(291,"presentation message loop ended (%ld)",result);
}

void InitializePresentation() {
    wchar_t mode[16]={}; DWORD n=GetEnvironmentVariableW(L"GH_PRESENT",mode,_countof(mode));
    if(n) {
        if(n>=_countof(mode) || (wcscmp(mode,L"1") && wcscmp(mode,L"hidden"))) Stop(291,"GH_PRESENT must be 1, hidden, or unset");
        g_present_enabled=true; g_present_hidden=!wcscmp(mode,L"hidden");
    }
    wchar_t keyboard[2]={}; n=GetEnvironmentVariableW(L"GH_KEYBOARD",keyboard,_countof(keyboard));
    if(n && (n!=1 || keyboard[0]!=L'1' || !g_present_enabled)) Stop(291,"GH_KEYBOARD=1 requires GH_PRESENT");
    g_keyboard_enabled=n!=0;
    if(!g_present_enabled) return;
    g_present_start_tick=GetTickCount64();
    InitializeCriticalSection(&g_present_lock); g_present_pixels.resize(640*480);
    g_present_started=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!g_present_started) Stop(291,"cannot create presentation startup event");
    HANDLE thread=CreateNativeThread(0x100000,PresentationThread,nullptr,false,false);
    if(!thread || WaitForSingleObject(g_present_started,10000)!=WAIT_OBJECT_0) Stop(291,"presentation startup failed");
    CloseHandle(thread); CloseHandle(g_present_started); g_present_started=nullptr;
    Log("presentation: %s, keyboard %s, waiting for authentic audio",g_present_hidden ? "hidden test" : "interactive",g_keyboard_enabled ? "enabled" : "disabled");
}

extern "C" void Nv2aOwnedFlush(uint32_t physical, uint32_t size);
extern "C" unsigned Nv2aGlRenderScale();
extern "C" bool Nv2aGlReadScanout(uint32_t physical, uint32_t pitch, uint32_t* pixels);
typedef void (*Nv2aPresentSink)(const uint32_t* top_down, unsigned w, unsigned h);
extern "C" bool Nv2aGlQueueScaledScanout(uint32_t addr, uint32_t pitch, Nv2aPresentSink sink);
// Runs on the renderer's presenter thread with a finished top-down frame.
void DeliverPresentedFrame(const uint32_t* pixels, unsigned w, unsigned h) {
    EnterCriticalSection(&g_present_lock);
    g_present_width = w; g_present_height = h;
    g_present_pixels.resize(size_t(w) * h);
    memcpy(g_present_pixels.data(), pixels, size_t(w) * h * 4);
    ++g_present_serial;
    LeaveCriticalSection(&g_present_lock);
    if (g_present_ready.load()) if (HWND wnd = g_present_window.load()) InvalidateRect(wnd, nullptr, FALSE);
}
void PresentScanout(uint32_t physical, uint32_t pitch) {
    if(!g_present_enabled) return;
    if(pitch<640*4 || uint64_t(physical)+uint64_t(479)*pitch+640*4>0x04000000)
        Stop(291,"presentation scan-out outside contiguous memory");
    // High resolution: copied and read back asynchronously (resolution.inc).
    static const bool sync_present = GetEnvironmentVariableW(L"GH_SYNC_PRESENT", nullptr, 0) != 0;
    if (!sync_present && Nv2aGlQueueScaledScanout(physical, pitch, DeliverPresentedFrame)) return;
    if(pitch<640*4 || uint64_t(physical)+uint64_t(479)*pitch+640*4>0x04000000)
        Stop(291,"presentation scan-out outside contiguous memory");
    static std::vector<uint32_t> scaled;
    unsigned scale = Nv2aGlRenderScale();
    scaled.resize(size_t(640) * 480 * scale * scale);
    bool high = Nv2aGlReadScanout(physical, pitch, scaled.data());
    if (!high) Nv2aOwnedFlush(physical,479*pitch+640*4);
    EnterCriticalSection(&g_present_lock);
    g_present_width = high ? 640 * scale : 640; g_present_height = high ? 480 * scale : 480;
    g_present_pixels.resize(size_t(g_present_width) * g_present_height);
    for(unsigned y=0;y<g_present_height;++y) {
        const void* row = high ? static_cast<const void*>(scaled.data() + size_t(g_present_height - 1 - y) * g_present_width) : At<void>(0x80000000+physical+y*pitch);
        memcpy(g_present_pixels.data()+size_t(y)*g_present_width,row,g_present_width*4);
    }
    ++g_present_serial;
    LeaveCriticalSection(&g_present_lock);
    // Repaint for every new frame (the 33 ms WM_TIMER only runs the start gate;
    // at the default 15.6 ms tick it fired every ~47 ms, capping display ~21 fps).
    if (g_present_ready.load()) if (HWND w = g_present_window.load()) InvalidateRect(w, nullptr, FALSE);
}
