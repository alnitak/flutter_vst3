#include "embedded_flutter_view.h"
#include "base/source/fobject.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/ustring.h"
#include <string>
#include <algorithm>
#include <vector>
#include <cmath>
#include <memory>
#include <variant>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

#if defined(FLUTTER_VST3_HAS_FLUTTER)
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <flutter/binary_messenger.h>
#include <flutter_windows.h>
#include "binary_messenger_impl.h"
#include "vst3_shared_memory.h"
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#endif
#endif

namespace Steinberg::Vst {

#ifndef __APPLE__

#if defined(_WIN32)

#if defined(FLUTTER_VST3_HAS_FLUTTER)
class WinFlutterHost {
public:
    HWND flutterHwnd{nullptr};
    HWND parentHwnd{nullptr};
    HMODULE flDll{nullptr};
    FlutterDesktopEngineRef engine{nullptr};
    FlutterDesktopViewControllerRef vc{nullptr};
    std::unique_ptr<flutter::BinaryMessengerImpl> binaryMessenger;
    std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel;
    EditController* controller{nullptr};

    UINT_PTR pendingTimerId{0};
    std::atomic<bool> mounted{false};
    std::atomic<bool> stopRequested{false};
    std::mutex channelMutex;

    static std::wstring getModuleDirectory() {
        HMODULE hModule = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&createEmbeddedFlutterView),
            &hModule);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(hModule, path, MAX_PATH);
        std::wstring p(path);
        size_t lastSlash = p.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
            return p.substr(0, lastSlash);
        }
        return L"";
    }

    static bool checkHasFlutter() {
        std::wstring moduleDir = getModuleDirectory();
        if (moduleDir.empty()) return false;
        std::wstring assetsPath = moduleDir + L"\\data\\flutter_assets";
        DWORD attr = GetFileAttributesW(assetsPath.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
    }

    bool init(HWND parent, EditController* ctrl, int w, int h) {
        controller = ctrl;
        parentHwnd = parent;
        stopRequested = false;
        mounted = false;

        std::wstring moduleDir = getModuleDirectory();
        if (moduleDir.empty()) return false;

        std::wstring dllPath = moduleDir + L"\\flutter_windows.dll";
        std::wstring assetsPath = moduleDir + L"\\data\\flutter_assets";
        DWORD attr = GetFileAttributesW(assetsPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return false;
        }

        SetDllDirectoryW(moduleDir.c_str());
        flDll = LoadLibraryW(dllPath.c_str());
        if (!flDll) return false;

        // Post-mount via thread timer so it executes on the main platform thread
        // AFTER the Dart FFI call (openEditorWindow) has returned to the event loop.
        UINT_PTR tid = SetTimer(nullptr, 0, 10, &WinFlutterHost::timerProc);
        if (tid == 0) return false;
        pendingTimerId = tid;
        registerTimer(tid, this);
        return true;
    }

    void mount() {
        if (mounted.load() || stopRequested.load() || !parentHwnd) return;

        std::wstring moduleDir = getModuleDirectory();
        std::wstring assetsPath = moduleDir + L"\\data\\flutter_assets";
        std::wstring icuPath = moduleDir + L"\\data\\icudtl.dat";

        FlutterDesktopEngineProperties props{};
        props.assets_path = assetsPath.c_str();
        props.icu_data_path = icuPath.c_str();
        props.dart_entrypoint = "echoMain";
        props.ui_thread_policy = RunOnPlatformThread;
        props.impeller_switch = DisabledImpeller;

        engine = FlutterDesktopEngineCreate(&props);
        if (!engine || stopRequested.load()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(channelMutex);
            setupMethodChannel();
        }

        if (!FlutterDesktopEngineRun(engine, "echoMain")) {
            FlutterDesktopEngineDestroy(engine);
            engine = nullptr;
            return;
        }

        RECT rc{};
        GetClientRect(parentHwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0) w = 600;
        if (h <= 0) h = 650;

        vc = FlutterDesktopViewControllerCreate(w, h, engine);
        if (!vc || stopRequested.load()) {
            engine = nullptr;
            return;
        }

        FlutterDesktopViewRef view = FlutterDesktopViewControllerGetView(vc);
        if (!view || stopRequested.load()) {
            FlutterDesktopViewControllerDestroy(vc);
            vc = nullptr;
            engine = nullptr;
            return;
        }

        flutterHwnd = FlutterDesktopViewGetHWND(view);
        if (!flutterHwnd || stopRequested.load()) {
            FlutterDesktopViewControllerDestroy(vc);
            vc = nullptr;
            engine = nullptr;
            return;
        }

        LONG_PTR curStyle = GetWindowLongPtrW(flutterHwnd, GWL_STYLE);
        curStyle = (curStyle & ~WS_POPUP) | WS_CHILD;
        SetWindowLongPtrW(flutterHwnd, GWL_STYLE, curStyle);

        SetParent(flutterHwnd, parentHwnd);
        SetWindowPos(flutterHwnd, nullptr, 0, 0, w, h, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ShowWindow(flutterHwnd, SW_SHOW);
        UpdateWindow(flutterHwnd);
        FlutterDesktopViewControllerForceRedraw(vc);

        mounted = true;
    }

    void shutdown() {
        stopRequested = true;
        if (pendingTimerId != 0) {
            KillTimer(nullptr, pendingTimerId);
            unregisterTimer(pendingTimerId);
            pendingTimerId = 0;
        }
        if (flutterHwnd) {
            SetParent(flutterHwnd, nullptr);
            flutterHwnd = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(channelMutex);
            channel.reset();
            binaryMessenger.reset();
        }
        if (vc) {
            FlutterDesktopViewControllerDestroy(vc);
            vc = nullptr;
            engine = nullptr;
        }
        if (flDll) {
            FreeLibrary(flDll);
            flDll = nullptr;
        }
        mounted = false;
        parentHwnd = nullptr;
    }

    void updateParam(Steinberg::Vst::ParamID pid, Steinberg::Vst::ParamValue val) {
        std::lock_guard<std::mutex> lock(channelMutex);
        if (!channel) return;
        flutter::EncodableMap args;
        args[flutter::EncodableValue("id")] = flutter::EncodableValue(static_cast<int64_t>(pid));
        args[flutter::EncodableValue("value")] = flutter::EncodableValue(static_cast<double>(val));
        channel->InvokeMethod("onParameterChanged", std::make_unique<flutter::EncodableValue>(args));
    }

private:
    void setupMethodChannel() {
        FlutterDesktopMessengerRef messenger = FlutterDesktopEngineGetMessenger(engine);
        if (!messenger) return;

        binaryMessenger = std::make_unique<flutter::BinaryMessengerImpl>(messenger);
        channel = std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            binaryMessenger.get(), "flutter_vst3/parameters",
            &flutter::StandardMethodCodec::GetInstance());

        auto weakCtrl = controller;
        channel->SetMethodCallHandler(
            [weakCtrl](const flutter::MethodCall<flutter::EncodableValue>& call,
                       std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
                if (call.method_name() == "setParameter" && weakCtrl) {
                    const auto* args = std::get_if<flutter::EncodableMap>(call.arguments());
                    if (!args) {
                        result->Error("INVALID_ARGUMENTS", "Expected map arguments");
                        return;
                    }
                    auto idIt = args->find(flutter::EncodableValue("id"));
                    auto valIt = args->find(flutter::EncodableValue("value"));
                    if (idIt != args->end() && valIt != args->end()) {
                        int pid = 0;
                        if (std::holds_alternative<int32_t>(idIt->second)) {
                            pid = std::get<int32_t>(idIt->second);
                        } else if (std::holds_alternative<int64_t>(idIt->second)) {
                            pid = static_cast<int>(std::get<int64_t>(idIt->second));
                        }
                        double val = 0.0;
                        if (std::holds_alternative<double>(valIt->second)) {
                            val = std::get<double>(valIt->second);
                        } else if (std::holds_alternative<int32_t>(valIt->second)) {
                            val = static_cast<double>(std::get<int32_t>(valIt->second));
                        } else if (std::holds_alternative<int64_t>(valIt->second)) {
                            val = static_cast<double>(std::get<int64_t>(valIt->second));
                        }
                        weakCtrl->beginEdit(pid);
                        weakCtrl->setParamNormalized(pid, val);
                        weakCtrl->performEdit(pid, val);
                        weakCtrl->endEdit(pid);
                        result->Success(flutter::EncodableValue(true));
                    } else {
                        result->Error("MISSING_PARAM", "id or value missing");
                    }
                } else if (call.method_name() == "getInitialParameters") {
                    flutter::EncodableMap map;
                    if (weakCtrl) {
                        int count = weakCtrl->getParameterCount();
                        for (int i = 0; i < count; ++i) {
                            Steinberg::Vst::ParameterInfo info{};
                            if (weakCtrl->getParameterInfo(i, info) == Steinberg::kResultTrue) {
                                map[flutter::EncodableValue(static_cast<int64_t>(info.id))] =
                                    flutter::EncodableValue(weakCtrl->getParamNormalized(info.id));
                            }
                        }
                    }
                    result->Success(flutter::EncodableValue(map));
                } else if (call.method_name() == "getSharedMemoryInfo") {
                    flutter::EncodableMap map;
                    const char* name = vst3_get_active_shm_name();
                    uintptr_t addr = reinterpret_cast<uintptr_t>(vst3_get_active_shm_ptr());
                    map[flutter::EncodableValue("shmName")] = flutter::EncodableValue(name ? std::string(name) : std::string(""));
                    map[flutter::EncodableValue("address")] = flutter::EncodableValue(static_cast<int64_t>(addr));
                    map[flutter::EncodableValue("totalSize")] = flutter::EncodableValue(static_cast<int64_t>(VST3_SHM_TOTAL_SIZE));
                    map[flutter::EncodableValue("maxSamples")] = flutter::EncodableValue(static_cast<int64_t>(VST3_SHM_MAX_SAMPLES));
                    result->Success(flutter::EncodableValue(map));
                } else {
                    result->NotImplemented();
                }
            });
    }

    static inline std::mutex sTimerMutex;
    static inline std::unordered_map<UINT_PTR, WinFlutterHost*> sPendingTimers;

    static void registerTimer(UINT_PTR id, WinFlutterHost* host) {
        std::lock_guard<std::mutex> lock(sTimerMutex);
        sPendingTimers[id] = host;
    }

    static void unregisterTimer(UINT_PTR id) {
        std::lock_guard<std::mutex> lock(sTimerMutex);
        sPendingTimers.erase(id);
    }

    static void CALLBACK timerProc(HWND hwnd, UINT msg, UINT_PTR id, DWORD time) {
        KillTimer(nullptr, id);
        WinFlutterHost* host = nullptr;
        {
            std::lock_guard<std::mutex> lock(sTimerMutex);
            auto it = sPendingTimers.find(id);
            if (it != sPendingTimers.end()) {
                host = it->second;
                sPendingTimers.erase(it);
            }
        }
        if (host) {
            host->pendingTimerId = 0;
            host->mount();
        }
    }
};
#endif // FLUTTER_VST3_HAS_FLUTTER

struct ParamRowControls {
    Steinberg::Vst::ParamID id{0};
    HWND nameHwnd{nullptr};
    HWND valHwnd{nullptr};
    HWND trackbarHwnd{nullptr};
};

class WinNativeEditorHost {
public:
    HWND hwnd{nullptr};
    EditController* controller{nullptr};
    std::string pluginName;
    std::vector<ParamRowControls> rows;
    HFONT fontTitle{nullptr};
    HFONT fontSubtitle{nullptr};
    HFONT fontLabel{nullptr};
    HFONT fontValue{nullptr};
    HFONT fontFooter{nullptr};
    HBRUSH bgBrush{nullptr};
    HPEN sepPen{nullptr};
    bool inScroll{false};

    static constexpr const wchar_t* kClassName = L"FlutterVst3NativeWin32View";

    static void registerClass(HINSTANCE hInst) {
        static bool registered = false;
        if (registered) return;

        INITCOMMONCONTROLSEX icex{};
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_BAR_CLASSES;
        InitCommonControlsEx(&icex);

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &WinNativeEditorHost::wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(23, 23, 28));
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    bool init(HWND parent, EditController* ctrl, const std::string& name, int w, int h) {
        controller = ctrl;
        pluginName = name;
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
        if (!hInst) hInst = GetModuleHandleW(nullptr);
        registerClass(hInst);

        hwnd = CreateWindowExW(
            0, kClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
            0, 0, w, h, parent, nullptr, hInst, this
        );
        return hwnd != nullptr;
    }

    void shutdown() {
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        rows.clear();
        if (bgBrush) { DeleteObject(bgBrush); bgBrush = nullptr; }
        if (sepPen) { DeleteObject(sepPen); sepPen = nullptr; }
        if (fontTitle) { DeleteObject(fontTitle); fontTitle = nullptr; }
        if (fontSubtitle) { DeleteObject(fontSubtitle); fontSubtitle = nullptr; }
        if (fontLabel) { DeleteObject(fontLabel); fontLabel = nullptr; }
        if (fontValue) { DeleteObject(fontValue); fontValue = nullptr; }
        if (fontFooter) { DeleteObject(fontFooter); fontFooter = nullptr; }
    }

    void updateParam(Steinberg::Vst::ParamID pid, Steinberg::Vst::ParamValue val) {
        if (!hwnd || inScroll) return;
        for (auto& row : rows) {
            if (row.id == pid) {
                int pos = static_cast<int>(std::round(val * 1000.0));
                SendMessageW(row.trackbarHwnd, TBM_SETPOS, TRUE, pos);
                updateValueLabel(row, val);
                break;
            }
        }
    }

    void updateValueLabel(const ParamRowControls& row, double val) {
        if (!row.valHwnd || !controller) return;
        Steinberg::Vst::String128 str{};
        std::wstring text;
        if (controller->getParamStringByValue(row.id, val, str) == Steinberg::kResultTrue) {
            char ascii[128] = {0};
            Steinberg::UString128(str).toAscii(ascii, sizeof(ascii));
            if (ascii[0] != '\0') {
                int needed = MultiByteToWideChar(CP_UTF8, 0, ascii, -1, nullptr, 0);
                if (needed > 1) {
                    text.resize(needed - 1);
                    MultiByteToWideChar(CP_UTF8, 0, ascii, -1, text.data(), needed);
                }
            }
        }
        if (text.empty()) {
            wchar_t buf[32];
            swprintf_s(buf, L"%.1f%%", val * 100.0);
            text = buf;
        }
        SetWindowTextW(row.valHwnd, text.c_str());
    }

private:
    static LRESULT CALLBACK wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return DefWindowProcW(hWnd, msg, wParam, lParam);
        }

        auto* self = reinterpret_cast<WinNativeEditorHost*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
        if (!self) return DefWindowProcW(hWnd, msg, wParam, lParam);

        switch (msg) {
        case WM_CREATE: {
            self->onCreate(hWnd);
            return 0;
        }
        case WM_HSCROLL: {
            HWND trackbar = (HWND)lParam;
            if (!trackbar || !self->controller) return 0;
            for (auto& row : self->rows) {
                if (row.trackbarHwnd == trackbar) {
                    self->inScroll = true;
                    int pos = (int)SendMessageW(trackbar, TBM_GETPOS, 0, 0);
                    double normVal = pos / 1000.0;
                    self->controller->beginEdit(row.id);
                    self->controller->setParamNormalized(row.id, normVal);
                    self->controller->performEdit(row.id, normVal);
                    self->controller->endEdit(row.id);
                    self->updateValueLabel(row, normVal);
                    self->inScroll = false;
                    break;
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            HWND ctrlHwnd = (HWND)lParam;
            SetBkMode(hdc, TRANSPARENT);
            bool isVal = false;
            for (const auto& r : self->rows) {
                if (r.valHwnd == ctrlHwnd) { isVal = true; break; }
            }
            if (isVal) {
                SetTextColor(hdc, RGB(64, 192, 255));
            } else {
                SetTextColor(hdc, RGB(230, 230, 235));
            }
            return (LRESULT)self->bgBrush;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, self->bgBrush);

            SelectObject(hdc, self->sepPen);
            MoveToEx(hdc, 24, 64, nullptr);
            LineTo(hdc, rc.right - 24, 64);

            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_SIZE: {
            int newW = LOWORD(lParam);
            for (auto& row : self->rows) {
                if (row.valHwnd) {
                    SetWindowPos(row.valHwnd, nullptr, newW - 134, 0, 110, 18, SWP_NOSIZE | SWP_NOZORDER);
                }
                if (row.trackbarHwnd) {
                    SetWindowPos(row.trackbarHwnd, nullptr, 24, 0, newW - 48, 24, SWP_NOMOVE | SWP_NOZORDER);
                }
            }
            InvalidateRect(hWnd, nullptr, FALSE);
            return 0;
        }
        }
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    void onCreate(HWND hWnd) {
        bgBrush = CreateSolidBrush(RGB(23, 23, 28));
        sepPen = CreatePen(PS_SOLID, 1, RGB(45, 45, 55));

        fontTitle = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontSubtitle = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontLabel = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        fontValue = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
        fontFooter = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hWnd, GWLP_HINSTANCE);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        if (w <= 0) w = 540;

        std::wstring widePluginName = L"VST3 Plugin";
        if (!pluginName.empty()) {
            int needed = MultiByteToWideChar(CP_UTF8, 0, pluginName.c_str(), -1, nullptr, 0);
            if (needed > 1) {
                widePluginName.resize(needed - 1);
                MultiByteToWideChar(CP_UTF8, 0, pluginName.c_str(), -1, widePluginName.data(), needed);
            }
        }
        HWND hTitle = CreateWindowExW(0, L"STATIC", widePluginName.c_str(),
                                      WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      24, 14, 350, 24, hWnd, nullptr, hInst, nullptr);
        SendMessageW(hTitle, WM_SETFONT, (WPARAM)fontTitle, TRUE);

        HWND hSub = CreateWindowExW(0, L"STATIC", L"flutter_vst3 \u2022 Pure Dart DSP & UI",
                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                    24, 40, 350, 18, hWnd, nullptr, hInst, nullptr);
        SendMessageW(hSub, WM_SETFONT, (WPARAM)fontSubtitle, TRUE);

        if (controller) {
            int count = controller->getParameterCount();
            int y = 78;
            for (int i = 0; i < count; ++i) {
                Steinberg::Vst::ParameterInfo info{};
                if (controller->getParameterInfo(i, info) != Steinberg::kResultTrue) continue;

                ParamRowControls row;
                row.id = info.id;

                char asciiTitle[128] = {0};
                Steinberg::UString128(info.title).toAscii(asciiTitle, sizeof(asciiTitle));
                std::wstring nameW = L"Param";
                if (asciiTitle[0] != '\0') {
                    int needed = MultiByteToWideChar(CP_UTF8, 0, asciiTitle, -1, nullptr, 0);
                    if (needed > 1) {
                        nameW.resize(needed - 1);
                        MultiByteToWideChar(CP_UTF8, 0, asciiTitle, -1, nameW.data(), needed);
                    }
                }
                row.nameHwnd = CreateWindowExW(0, L"STATIC", nameW.c_str(),
                                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                                               24, y, w - 160, 18, hWnd, nullptr, hInst, nullptr);
                SendMessageW(row.nameHwnd, WM_SETFONT, (WPARAM)fontLabel, TRUE);

                row.valHwnd = CreateWindowExW(0, L"STATIC", L"",
                                              WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                              w - 134, y, 110, 18, hWnd, nullptr, hInst, nullptr);
                SendMessageW(row.valHwnd, WM_SETFONT, (WPARAM)fontValue, TRUE);

                double currentVal = controller->getParamNormalized(info.id);
                int initPos = static_cast<int>(std::round(currentVal * 1000.0));
                row.trackbarHwnd = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                                   WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
                                                   24, y + 22, w - 48, 24, hWnd,
                                                   (HMENU)(UINT_PTR)(10000 + i), hInst, nullptr);
                SendMessageW(row.trackbarHwnd, TBM_SETRANGEMIN, FALSE, 0);
                SendMessageW(row.trackbarHwnd, TBM_SETRANGEMAX, FALSE, 1000);
                SendMessageW(row.trackbarHwnd, TBM_SETPOS, TRUE, initPos);

                updateValueLabel(row, currentVal);
                rows.push_back(row);

                y += 56;
            }

            HWND hFooter = CreateWindowExW(0, L"STATIC", L"Preallocated Zero-Copy Shared Memory DSP",
                                           WS_CHILD | WS_VISIBLE | SS_CENTER,
                                           24, y + 10, w - 48, 16, hWnd, nullptr, hInst, nullptr);
            SendMessageW(hFooter, WM_SETFONT, (WPARAM)fontFooter, TRUE);
        }
    }
};

#elif defined(__linux__)
#include <dlfcn.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>

/// Forward declarations for Linux Flutter embedding types.
typedef struct _FlDartProject FlDartProject;
typedef struct _FlView FlView;

struct LinuxFlutterHost {
    void* flDll{nullptr};
    GtkWidget* plug{nullptr};
    FlView* flView{nullptr};

    bool init(void* parentX11, int w, int h) {
        if (!gtk_init_check(nullptr, nullptr)) return false;
        flDll = dlopen("libflutter_linux_gtk.so", RTLD_LAZY);
        if (!flDll) return false;

        auto fnProjectNew = (FlDartProject* (*)())dlsym(flDll, "fl_dart_project_new");
        auto fnViewNew = (FlView* (*)(FlDartProject*))dlsym(flDll, "fl_view_new");
        if (!fnProjectNew || !fnViewNew) return false;

        FlDartProject* proj = fnProjectNew();
        flView = fnViewNew(proj);
        plug = gtk_plug_new((Window)(uintptr_t)parentX11);
        gtk_widget_set_size_request(plug, w, h);
        gtk_container_add(GTK_CONTAINER(plug), GTK_WIDGET(flView));
        gtk_widget_show_all(plug);
        return true;
    }

    void shutdown() {
        if (plug) { gtk_widget_destroy(plug); plug = nullptr; flView = nullptr; }
        if (flDll) { dlclose(flDll); flDll = nullptr; }
    }
};
#endif

/// Cross-platform IPlugView implementation for Windows (HWND) and Linux (X11).
class EmbeddedFlutterView : public IPlugView, public FObject, public IEmbeddedFlutterView {
public:
    EmbeddedFlutterView(EditController* c, const char* name, IEmbeddedFlutterView** holder = nullptr)
        : controller_(c), pluginName_(name ? name : "VST3 Plugin"), viewHolder_(holder) {
#if defined(_WIN32) && defined(FLUTTER_VST3_HAS_FLUTTER)
        hasFlutter_ = WinFlutterHost::checkHasFlutter();
#endif
        if (viewHolder_) *viewHolder_ = this;
    }

    ~EmbeddedFlutterView() override {
        cleanup();
        if (viewHolder_ && *viewHolder_ == this) *viewHolder_ = nullptr;
    }

    void updateParameter(ParamID paramId, ParamValue value) override {
#if defined(_WIN32)
#if defined(FLUTTER_VST3_HAS_FLUTTER)
        if (hasFlutter_) {
            winFlutterHost_.updateParam(paramId, value);
            return;
        }
#endif
        winHost_.updateParam(paramId, value);
#endif
    }

    void onControllerClosed() override {
        controller_ = nullptr;
        viewHolder_ = nullptr;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
#if defined(_WIN32)
        return (type && strcmp(type, kPlatformTypeHWND) == 0) ? kResultTrue : kResultFalse;
#else
        return (type && strcmp(type, kPlatformTypeX11EmbedWindowID) == 0) ? kResultTrue : kResultFalse;
#endif
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (!parent || !type) return kInvalidArgument;
        parentView_ = parent;
#if defined(_WIN32)
#if defined(FLUTTER_VST3_HAS_FLUTTER)
        if (hasFlutter_) {
            bool ok = winFlutterHost_.init((HWND)parent, controller_, 600, 650);
            if (!ok) {
                hasFlutter_ = false;
            }
        }
#endif
        if (!hasFlutter_) {
            int count = controller_ ? controller_->getParameterCount() : 0;
            int h = (std::max)(280, 74 + count * 56 + 36);
            winHost_.init((HWND)parent, controller_, pluginName_, 540, h);
        }
#elif defined(__linux__)
        hasFlutter_ = linuxHost_.init(parent, 600, 650);
#endif
        return kResultTrue;
    }

    tresult PLUGIN_API removed() override {
        cleanup();
        if (viewHolder_ && *viewHolder_ == this) *viewHolder_ = nullptr;
        return kResultTrue;
    }

    tresult PLUGIN_API getSize(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        int count = controller_ ? controller_->getParameterCount() : 0;
        size->left = 0;
        size->top = 0;
        size->right = hasFlutter_ ? 600 : 540;
        size->bottom = hasFlutter_ ? 650 : (std::max)(280, 74 + count * 56 + 36);
        return kResultTrue;
    }

    tresult PLUGIN_API onSize(ViewRect* r) override {
#if defined(_WIN32)
        if (r) {
#if defined(FLUTTER_VST3_HAS_FLUTTER)
            if (hasFlutter_ && winFlutterHost_.flutterHwnd) {
                SetWindowPos(winFlutterHost_.flutterHwnd, nullptr, 0, 0, r->getWidth(), r->getHeight(), SWP_NOMOVE | SWP_NOZORDER);
                return kResultTrue;
            }
#endif
            if (winHost_.hwnd) {
                SetWindowPos(winHost_.hwnd, nullptr, 0, 0, r->getWidth(), r->getHeight(), SWP_NOMOVE | SWP_NOZORDER);
            }
        }
#endif
        return kResultTrue;
    }

    tresult PLUGIN_API setFrame(IPlugFrame* frame) override { return kResultTrue; }
    tresult PLUGIN_API canResize() override { return kResultFalse; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect*) override { return kResultTrue; }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onFocus(TBool) override { return kResultTrue; }

    REFCOUNT_METHODS(FObject)

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, IPlugView::iid, IPlugView)
        return FObject::queryInterface(iid, obj);
    }

private:
    void cleanup() {
#if defined(_WIN32)
#if defined(FLUTTER_VST3_HAS_FLUTTER)
        if (hasFlutter_) {
            winFlutterHost_.shutdown();
        }
#endif
        winHost_.shutdown();
#elif defined(__linux__)
        linuxHost_.shutdown();
#endif
        parentView_ = nullptr;
        hasFlutter_ = false;
    }

    EditController* controller_{nullptr};
    std::string pluginName_;
    void* parentView_{nullptr};
    IEmbeddedFlutterView** viewHolder_{nullptr};
    bool hasFlutter_{false};
#if defined(_WIN32)
#if defined(FLUTTER_VST3_HAS_FLUTTER)
    WinFlutterHost winFlutterHost_;
#endif
    WinNativeEditorHost winHost_;
#elif defined(__linux__)
    LinuxFlutterHost linuxHost_;
#endif
};

IPlugView* createEmbeddedFlutterView(
    EditController* controller,
    const char* pluginName,
    IEmbeddedFlutterView** outViewPtr) {
    return new EmbeddedFlutterView(controller, pluginName, outViewPtr);
}

#endif // !__APPLE__

} // namespace Steinberg::Vst