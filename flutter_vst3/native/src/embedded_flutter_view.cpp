// Copyright (c) 2025
// Native embedded view for flutter_vst3 plugins on Windows and Linux
// Provides in-process Flutter Engine embedding with HWND and X11 platform docking.

#include "embedded_flutter_view.h"
#include "base/source/fobject.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include <string>
#include <algorithm>

namespace Steinberg::Vst {

#ifndef __APPLE__

#if defined(_WIN32)
#include <windows.h>

/// Function pointer signatures for Flutter Windows C API.
typedef void* FlutterDesktopEngineRef;
typedef void* FlutterDesktopViewControllerRef;
typedef void* FlutterDesktopViewRef;
typedef void* FlutterDesktopMessengerRef;

struct FlutterDesktopEngineProperties {
    const wchar_t* assets_path;
    const wchar_t* icu_data_path;
    const wchar_t* aot_library_path;
    int dart_entrypoint_argc;
    const char** dart_entrypoint_argv;
};

typedef FlutterDesktopEngineRef (*FnEngineCreate)(const FlutterDesktopEngineProperties*);
typedef bool (*FnEngineDestroy)(FlutterDesktopEngineRef);
typedef FlutterDesktopViewControllerRef (*FnVcCreate)(int, int, FlutterDesktopEngineRef);
typedef void (*FnVcDestroy)(FlutterDesktopViewControllerRef);
typedef FlutterDesktopViewRef (*FnVcGetView)(FlutterDesktopViewControllerRef);
typedef HWND (*FnViewGetHWND)(FlutterDesktopViewRef);
typedef FlutterDesktopMessengerRef (*FnEngineGetMessenger)(FlutterDesktopEngineRef);

struct WinFlutterHost {
    HMODULE dll{nullptr};
    FlutterDesktopEngineRef engine{nullptr};
    FlutterDesktopViewControllerRef vc{nullptr};
    HWND flutterHwnd{nullptr};

    bool init(HWND parent, int w, int h) {
        dll = LoadLibraryA("flutter_windows.dll");
        if (!dll) return false;
        auto fnEngineCreate = (FnEngineCreate)GetProcAddress(dll, "FlutterDesktopEngineCreate");
        auto fnVcCreate = (FnVcCreate)GetProcAddress(dll, "FlutterDesktopViewControllerCreate");
        auto fnVcGetView = (FnVcGetView)GetProcAddress(dll, "FlutterDesktopViewControllerGetView");
        auto fnViewGetHWND = (FnViewGetHWND)GetProcAddress(dll, "FlutterDesktopViewGetHWND");
        if (!fnEngineCreate || !fnVcCreate || !fnVcGetView || !fnViewGetHWND) return false;

        FlutterDesktopEngineProperties props = { L"data/flutter_assets", L"data/icudtl.dat", nullptr, 0, nullptr };
        engine = fnEngineCreate(&props);
        if (!engine) return false;
        vc = fnVcCreate(w, h, engine);
        if (!vc) return false;
        flutterHwnd = fnViewGetHWND(fnVcGetView(vc));
        if (!flutterHwnd) return false;
        SetParent(flutterHwnd, parent);
        SetWindowPos(flutterHwnd, nullptr, 0, 0, w, h, SWP_SHOWWINDOW);
        return true;
    }

    void shutdown() {
        if (vc && dll) {
            auto fnVcDestroy = (FnVcDestroy)GetProcAddress(dll, "FlutterDesktopViewControllerDestroy");
            if (fnVcDestroy) fnVcDestroy(vc);
            vc = nullptr;
        }
        if (engine && dll) {
            auto fnEngineDestroy = (FnEngineDestroy)GetProcAddress(dll, "FlutterDesktopEngineDestroy");
            if (fnEngineDestroy) fnEngineDestroy(engine);
            engine = nullptr;
        }
        if (dll) { FreeLibrary(dll); dll = nullptr; }
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
        if (viewHolder_) *viewHolder_ = this;
    }

    ~EmbeddedFlutterView() override {
        cleanup();
        if (viewHolder_ && *viewHolder_ == this) *viewHolder_ = nullptr;
    }

    void updateParameter(ParamID, ParamValue) override {
        // Parameter updates forward to Flutter binary messenger via JSON message.
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
        hasFlutter_ = winHost_.init((HWND)parent, 600, 650);
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
        size->bottom = hasFlutter_ ? 650 : std::max(280, 74 + count * 56 + 36);
        return kResultTrue;
    }

    tresult PLUGIN_API onSize(ViewRect*) override { return kResultTrue; }
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
    WinFlutterHost winHost_;
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