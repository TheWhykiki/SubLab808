#include "NativeFilePanel.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>
#import <AppKit/AppKit.h>

namespace
{
NSString* checkedUTF8(const char* text)
{
    auto* result = [NSString stringWithUTF8String:text];
    if (result == nil) throw std::runtime_error("Native panel test received invalid UTF-8");
    return result;
}

NSSavePanel* resolvePanel(void* identity)
{
    // JUCE uses releasedWhenClosed for these panels, so retaining one here
    // would change the lifecycle that this bridge is meant to observe.
    for (NSWindow* window in [NSApp windows])
        if ((void*) window == identity && [window isKindOfClass:[NSSavePanel class]])
            return (NSSavePanel*) window;
    return nil;
}
}

NativeFilePanel::NativeFilePanel(void* nativePanel) : panel(nativePanel) {}
NativeFilePanel::~NativeFilePanel() = default;
bool NativeFilePanel::isAlive() const
{
    @autoreleasepool
    {
        return resolvePanel(panel) != nil;
    }
}
void NativeFilePanel::prepareTestApplication()
{
    // ScopedJuceInitialiser_GUI in a console test does not provide an active
    // regular app. Complete the launch sequence once before the bounded event
    // dispatcher is used. Do not re-enter the unbounded top-level NSApp run
    // while an asynchronous native-panel completion is still retiring.
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular)
            throw std::runtime_error("NATIVE_PANEL_SETUP: test application cannot become a regular GUI process");
        [NSApp finishLaunching];
    }
}
void NativeFilePanel::disableAutomaticHostWindowAnimations(void* nativeView)
{
    @autoreleasepool
    {
        auto* view = (NSView*) nativeView;
        NSWindow* window = view == nil || ! [view isKindOfClass:[NSView class]] ? nil : [view window];
        if (window == nil || ! [[window title] isEqualToString:@"Preset UI Tests"])
            throw std::runtime_error("Native panel tests could not resolve their synthetic host window");
        [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
    }
}
void NativeFilePanel::dispatchEventsFor(int millisecondsToRunFor)
{
    if (millisecondsToRunFor < 0)
        throw std::runtime_error("Native panel event dispatch received a negative duration");

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(millisecondsToRunFor);
    while (std::chrono::steady_clock::now() < deadline)
    {
        @autoreleasepool
        {
            if (![NSThread isMainThread])
                throw std::runtime_error("Native panel tests require main-thread event dispatch");
            for (NSRunLoopMode mode in @[ NSDefaultRunLoopMode,
                                          NSModalPanelRunLoopMode,
                                          NSEventTrackingRunLoopMode ])
            {
                const auto remaining = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
                if (remaining <= 0.0) break;
                CFRunLoopRunInMode((__bridge CFStringRef) mode, std::min(remaining, 0.001), true);
                if (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                         untilDate:[NSDate distantPast]
                                                            inMode:mode
                                                           dequeue:YES])
                    [NSApp sendEvent:event];
            }
        }
    }
}
std::unique_ptr<NativeFilePanel> NativeFilePanel::findVisible(bool importing, const char* title)
{
    @autoreleasepool
    {
        for (NSWindow* window in [NSApp windows])
        {
            if (![window isKindOfClass:[NSSavePanel class]] || ![window isVisible])
                continue;
            auto* candidate = (NSSavePanel*) window;
            const bool isOpen = [candidate isKindOfClass:[NSOpenPanel class]];
            if (isOpen == importing && [[candidate title] isEqualToString:checkedUTF8(title)])
                return std::unique_ptr<NativeFilePanel>(new NativeFilePanel(candidate));
        }
        return {};
    }
}
int NativeFilePanel::visibleCount()
{
    @autoreleasepool
    {
        int count = 0;
        for (NSWindow* window in [NSApp windows])
            if ([window isKindOfClass:[NSSavePanel class]] && [window isVisible]) ++count;
        return count;
    }
}
bool NativeFilePanel::isVisible() const
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel);
        return candidate != nil && [candidate isVisible];
    }
}
bool NativeFilePanel::hasDelegate() const
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel);
        return candidate != nil && [candidate delegate] != nil;
    }
}
std::string NativeFilePanel::className() const
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel);
        if (candidate == nil) throw std::runtime_error("Native panel disappeared before inspection");
        return NSStringFromClass([candidate class]).UTF8String;
    }
}
void NativeFilePanel::useFixtureLocation(const std::string& directory, const std::string& filename)
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel);
        if (candidate == nil) throw std::runtime_error("Native panel disappeared before fixture setup");
        [candidate setDirectoryURL:[NSURL fileURLWithPath:checkedUTF8(directory.c_str())
                                             isDirectory:YES]];
        [candidate setNameFieldStringValue:checkedUTF8(filename.c_str())];
    }
}
