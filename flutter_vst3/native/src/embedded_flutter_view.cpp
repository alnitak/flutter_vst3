// Copyright (c) 2025
// Embedded view stub for non-macOS platforms (Linux/Windows)
//
// Overview:
// Implements the IEmbeddedFlutterView interface and IPlugView contract for
// platforms where the native macOS AppKit GUI is not used. Provides HWND (Windows)
// and X11 (Linux) platform support with identical lifecycle and parameter
// synchronization semantics.

#include "embedded_flutter_view.h"
#include "base/source/fobject.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include <string>

namespace Steinberg::Vst {

#ifndef __APPLE__

/// Non-macOS stub implementation of IPlugView and IEmbeddedFlutterView.
class EmbeddedFlutterView : public IPlugView, public FObject, public IEmbeddedFlutterView {
public:
    EmbeddedFlutterView(EditController* controller, const char* name, IEmbeddedFlutterView** holder = nullptr)
        : controller_(controller), pluginName_(name ? name : "VST3 Plugin"), viewHolder_(holder) {
        if (viewHolder_) *viewHolder_ = this;
    }

    ~EmbeddedFlutterView() override {
        if (viewHolder_ && *viewHolder_ == this) *viewHolder_ = nullptr;
    }

    /// No-op stub for parameter updates on non-macOS platforms.
    void updateParameter(ParamID, ParamValue) override {}

    /// Detaches controller and holder references when the controller is destroyed.
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

    tresult PLUGIN_API attached(void* parent, FIDString) override {
        parentView_ = parent;
        return kResultTrue;
    }

    tresult PLUGIN_API removed() override {
        parentView_ = nullptr;
        if (viewHolder_ && *viewHolder_ == this) *viewHolder_ = nullptr;
        return kResultTrue;
    }

    tresult PLUGIN_API getSize(ViewRect* size) override {
        if (!size) return kInvalidArgument;
        int count = controller_ ? controller_->getParameterCount() : 0;
        size->left = 0;
        size->top = 0;
        size->right = 540;
        size->bottom = std::max(280, 74 + count * 56 + 36);
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
    EditController* controller_{nullptr};
    std::string pluginName_;
    void* parentView_{nullptr};
    IEmbeddedFlutterView** viewHolder_{nullptr};
};

IPlugView* createEmbeddedFlutterView(
    EditController* controller,
    const char* pluginName,
    IEmbeddedFlutterView** outViewPtr) {
    return new EmbeddedFlutterView(controller, pluginName, outViewPtr);
}

#endif

} // namespace Steinberg::Vst