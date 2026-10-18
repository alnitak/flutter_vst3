// Copyright (c) 2025
// Native Cocoa/AppKit embedded view for flutter_vst3 plugins
// Provides a sleek, zero-dependency pro-audio GUI in host DAWs

#import <Cocoa/Cocoa.h>
#include "pluginterfaces/gui/iplugview.h"
#include "embedded_flutter_view.h"
#include "base/source/fobject.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/base/ustring.h"
#include <cmath>
#include <string>

/// Custom NSView container with flipped coordinates (top-left origin (0,0)).
/// Standard AppKit views use bottom-left origin; flipping simplifies layout calculations.
@interface Vst3FlippedView : NSView
@end

@implementation Vst3FlippedView
- (BOOL)isFlipped {
    return YES;
}
@end

/// Action delegate handling user slider interactions and parameter display updates.
@interface Vst3PluginEditorDelegate : NSObject
@property (nonatomic, assign) Steinberg::Vst::EditController* controller;
@property (nonatomic, strong) NSMutableArray<NSSlider*>* sliders;
@property (nonatomic, strong) NSMutableArray<NSTextField*>* valueLabels;

/// Invoked when the user drags a slider in the GUI window.
- (void)onSliderChanged:(NSSlider*)sender;

/// Called when a parameter is updated externally (DAW host, preset, automation).
- (void)updateParamId:(Steinberg::Vst::ParamID)paramId withValue:(Steinberg::Vst::ParamValue)value;
@end

@implementation Vst3PluginEditorDelegate
/// Handles slider dragging: notifies the controller and the DAW host automation queue.
- (void)onSliderChanged:(NSSlider*)sender {
    if (!_controller) return;
    Steinberg::Vst::ParamID paramId = static_cast<Steinberg::Vst::ParamID>(sender.tag);
    double val = [sender doubleValue];

    // VST3 edit transaction: beginEdit -> setParamNormalized -> performEdit -> endEdit
    // performEdit forwards the change to both the DAW host and the audio DSP thread.
    _controller->beginEdit(paramId);
    _controller->setParamNormalized(paramId, val);
    _controller->performEdit(paramId, val);
    _controller->endEdit(paramId);
    [self refreshLabelForParam:paramId value:val slider:sender];
}

/// Formats the parameter value into a display string using controller metadata.
- (void)refreshLabelForParam:(Steinberg::Vst::ParamID)paramId value:(double)val slider:(NSSlider*)slider {
    Steinberg::Vst::String128 str;
    _controller->getParamStringByValue(paramId, val, str);
    char ascii[128] = {0};
    Steinberg::UString128(str).toAscii(ascii, sizeof(ascii));
    NSString* text = (ascii[0] != '\0')
        ? [NSString stringWithUTF8String:ascii]
        : [NSString stringWithFormat:@"%.1f%%", val * 100.0];
    NSInteger idx = [_sliders indexOfObject:slider];
    if (idx != NSNotFound && idx < static_cast<NSInteger>(_valueLabels.count)) {
        [_valueLabels[idx] setStringValue:text];
    }
}

/// Updates slider position and text label when the parameter changes externally.
/// Includes a deadband check (1e-5) to avoid circular feedback loops.
- (void)updateParamId:(Steinberg::Vst::ParamID)paramId withValue:(Steinberg::Vst::ParamValue)value {
    for (NSUInteger i = 0; i < _sliders.count; ++i) {
        if (static_cast<Steinberg::Vst::ParamID>(_sliders[i].tag) == paramId) {
            if (std::abs([_sliders[i] doubleValue] - value) > 1e-5) {
                [_sliders[i] setDoubleValue:value];
                if (_controller && i < _valueLabels.count) {
                    [self refreshLabelForParam:paramId value:value slider:_sliders[i]];
                }
            }
            break;
        }
    }
}
@end

namespace Steinberg::Vst {

/// Native AppKit implementation of IPlugView for flutter_vst3 plugins.
/// Implements IEmbeddedFlutterView for bidirectional controller parameter sync.
class EmbeddedFlutterView : public Steinberg::IPlugView,
                            public Steinberg::FObject,
                            public IEmbeddedFlutterView {
public:
    EmbeddedFlutterView(EditController* controller, const char* name, IEmbeddedFlutterView** holder = nullptr)
        : controller_(controller),
          pluginName_(name ? name : "VST3 Plugin"),
          viewHolder_(holder) {
        if (viewHolder_) {
            *viewHolder_ = this;
        }
    }

    ~EmbeddedFlutterView() override {
        cleanup();
        if (viewHolder_ && *viewHolder_ == this) {
            *viewHolder_ = nullptr;
        }
    }

    /// Called if the EditController is destroyed before this view.
    void onControllerClosed() override {
        controller_ = nullptr;
        if (delegate_) {
            delegate_.controller = nullptr;
        }
        viewHolder_ = nullptr;
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return (type && strcmp(type, kPlatformTypeNSView) == 0) ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API attached(void* parent, FIDString type) override {
        if (!parent || !type || strcmp(type, kPlatformTypeNSView) != 0) {
            return kInvalidArgument;
        }
        parentView_ = parent;
        buildUI();
        return kResultTrue;
    }

    tresult PLUGIN_API removed() override {
        cleanup();
        if (viewHolder_ && *viewHolder_ == this) {
            *viewHolder_ = nullptr;
        }
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
    tresult PLUGIN_API setFrame(IPlugFrame* frame) override { frame_ = frame; return kResultTrue; }
    tresult PLUGIN_API canResize() override { return kResultFalse; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect*) override { return kResultTrue; }
    tresult PLUGIN_API onWheel(float) override { return kResultFalse; }
    tresult PLUGIN_API onKeyDown(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onKeyUp(char16, int16, int16) override { return kResultFalse; }
    tresult PLUGIN_API onFocus(TBool) override { return kResultTrue; }

    /// Receives parameter updates from EditController and dispatches to macOS main thread.
    void updateParameter(ParamID id, ParamValue value) override {
        if (!delegate_) return;
        dispatch_async(dispatch_get_main_queue(), ^{
            [delegate_ updateParamId:id withValue:value];
        });
    }


    REFCOUNT_METHODS(Steinberg::FObject)

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override {
        QUERY_INTERFACE(iid, obj, Steinberg::IPlugView::iid, Steinberg::IPlugView)
        return Steinberg::FObject::queryInterface(iid, obj);
    }

private:
    void cleanup() {
        if (containerView_) {
            [containerView_ removeFromSuperview];
            containerView_ = nil;
        }
        delegate_ = nil;
        parentView_ = nullptr;
    }

    void buildUI() {
        NSView* parent = (__bridge NSView*)parentView_;
        containerView_ = [[Vst3FlippedView alloc] initWithFrame:parent.bounds];
        containerView_.wantsLayer = YES;
        containerView_.layer.backgroundColor = [NSColor colorWithCalibratedRed:0.09 green:0.09 blue:0.11 alpha:1.0].CGColor;
        containerView_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        delegate_ = [[Vst3PluginEditorDelegate alloc] init];
        delegate_.controller = controller_;
        delegate_.sliders = [NSMutableArray array];
        delegate_.valueLabels = [NSMutableArray array];

        buildHeader(containerView_);
        buildParameterRows(containerView_);
        buildFooter(containerView_);
        [parent addSubview:containerView_];
    }

    void buildHeader(NSView* parent) {
        CGFloat width = parent.bounds.size.width;
        NSView* header = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, 64)];
        header.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;

        NSTextField* titleLabel = [NSTextField labelWithString:[NSString stringWithUTF8String:pluginName_.c_str()]];
        titleLabel.frame = NSMakeRect(24, 12, 300, 24);
        titleLabel.font = [NSFont systemFontOfSize:17 weight:NSFontWeightBold];
        titleLabel.textColor = [NSColor whiteColor];
        [header addSubview:titleLabel];

        NSTextField* subLabel = [NSTextField labelWithString:@"flutter_vst3 • Pure Dart DSP & UI"];
        subLabel.frame = NSMakeRect(24, 38, 300, 16);
        subLabel.font = [NSFont systemFontOfSize:11 weight:NSFontWeightMedium];
        subLabel.textColor = [NSColor colorWithCalibratedWhite:0.6 alpha:1.0];
        [header addSubview:subLabel];

        NSTextField* badge = [NSTextField labelWithString:@"VST3 | 64-bit"];
        badge.frame = NSMakeRect(width - 130, 22, 106, 20);
        badge.alignment = NSTextAlignmentCenter;
        badge.font = [NSFont systemFontOfSize:10 weight:NSFontWeightSemibold];
        badge.textColor = [NSColor colorWithCalibratedRed:0.2 green:0.85 blue:0.5 alpha:1.0];
        badge.wantsLayer = YES;
        badge.layer.backgroundColor = [NSColor colorWithCalibratedRed:0.15 green:0.15 blue:0.18 alpha:1.0].CGColor;
        badge.layer.cornerRadius = 10;
        badge.autoresizingMask = NSViewMinXMargin;
        [header addSubview:badge];

        NSBox* sep = [[NSBox alloc] initWithFrame:NSMakeRect(0, 63, width, 1)];
        sep.boxType = NSBoxSeparator;
        sep.autoresizingMask = NSViewWidthSizable;
        [header addSubview:sep];
        [parent addSubview:header];
    }

    void buildParameterRows(NSView* parent) {
        if (!controller_) return;
        int32 count = controller_->getParameterCount();
        CGFloat width = parent.bounds.size.width;
        CGFloat y = 74.0;
        for (int32 i = 0; i < count; ++i) {
            ParameterInfo info;
            if (controller_->getParameterInfo(i, info) != kResultTrue) continue;
            buildSingleRow(parent, info, width, y);
            y += 56.0;
        }
    }

    void buildSingleRow(NSView* parent, const ParameterInfo& info, CGFloat width, CGFloat y) {
        char asciiTitle[128] = {0};
        Steinberg::UString128(info.title).toAscii(asciiTitle, sizeof(asciiTitle));
        NSString* name = (asciiTitle[0] != '\0') ? [NSString stringWithUTF8String:asciiTitle] : [NSString stringWithFormat:@"Param %d", info.id];

        NSTextField* nameLabel = [NSTextField labelWithString:name];
        nameLabel.frame = NSMakeRect(24, y, width - 160, 18);
        nameLabel.font = [NSFont systemFontOfSize:13 weight:NSFontWeightSemibold];
        nameLabel.textColor = [NSColor colorWithCalibratedWhite:0.92 alpha:1.0];
        [parent addSubview:nameLabel];

        double val = controller_->getParamNormalized(info.id);
        Steinberg::Vst::String128 str;
        controller_->getParamStringByValue(info.id, val, str);
        char asciiVal[128] = {0};
        Steinberg::UString128(str).toAscii(asciiVal, sizeof(asciiVal));
        NSString* valText = (asciiVal[0] != '\0') ? [NSString stringWithUTF8String:asciiVal] : [NSString stringWithFormat:@"%.1f%%", val * 100.0];

        NSTextField* valLabel = [NSTextField labelWithString:valText];
        valLabel.frame = NSMakeRect(width - 134, y, 110, 18);
        valLabel.font = [NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium];
        valLabel.textColor = [NSColor colorWithCalibratedRed:0.25 green:0.75 blue:1.0 alpha:1.0];
        valLabel.alignment = NSTextAlignmentRight;
        valLabel.autoresizingMask = NSViewMinXMargin;
        [parent addSubview:valLabel];
        [delegate_.valueLabels addObject:valLabel];

        NSSlider* slider = [NSSlider sliderWithValue:val minValue:0.0 maxValue:1.0 target:delegate_ action:@selector(onSliderChanged:)];
        slider.frame = NSMakeRect(24, y + 22, width - 48, 20);
        slider.continuous = YES;
        slider.tag = info.id;
        slider.autoresizingMask = NSViewWidthSizable;
        [slider setTarget:delegate_];
        [slider setAction:@selector(onSliderChanged:)];
        [parent addSubview:slider];
        [delegate_.sliders addObject:slider];
    }

    void buildFooter(NSView* parent) {
        CGFloat width = parent.bounds.size.width;
        CGFloat height = parent.bounds.size.height;
        NSTextField* footer = [NSTextField labelWithString:@"Preallocated Zero-Copy Shared Memory DSP"];
        footer.frame = NSMakeRect(24, height - 24, width - 48, 16);
        footer.alignment = NSTextAlignmentCenter;
        footer.font = [NSFont systemFontOfSize:10 weight:NSFontWeightRegular];
        footer.textColor = [NSColor colorWithCalibratedWhite:0.4 alpha:1.0];
        footer.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
        [parent addSubview:footer];
    }

    EditController* controller_{nullptr};
    std::string pluginName_;
    void* parentView_{nullptr};
    IPlugFrame* frame_{nullptr};
    Vst3FlippedView* containerView_{nil};
    Vst3PluginEditorDelegate* delegate_{nil};
    IEmbeddedFlutterView** viewHolder_{nullptr};
};

Steinberg::IPlugView* createEmbeddedFlutterView(
    EditController* controller,
    const char* pluginName,
    IEmbeddedFlutterView** outViewPtr) {
    return new EmbeddedFlutterView(controller, pluginName, outViewPtr);
}

} // namespace Steinberg::Vst