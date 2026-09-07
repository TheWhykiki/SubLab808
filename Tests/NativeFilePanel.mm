#include "NativeFilePanel.h"
#include <stdexcept>
#import <AppKit/AppKit.h>

namespace
{
bool activationLifecycleEventsAllowed = true;

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

    @autoreleasepool
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("Native panel tests require main-thread event dispatch");

        // Process one run-loop source boundary, then let the caller re-check its
        // predicate. Sending NSEvents here can enter an unbounded synchronous
        // AppKit callback after a native panel has closed.
        if (millisecondsToRunFor > 0)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                               static_cast<CFTimeInterval>(millisecondsToRunFor) * 0.001, true);
    }
}
void NativeFilePanel::dispatchActivationEventsFor(int millisecondsToRunFor)
{
    if (millisecondsToRunFor < 0)
        throw std::runtime_error("Native panel activation dispatch received a negative duration");

    @autoreleasepool
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("Native panel tests require main-thread event dispatch");

        // A lifecycle NSEvent is safe only while bootstrapping the process's
        // first test window. Once any native panel has been observed, never
        // re-enter AppKit through sendEvent; later windows need source delivery
        // only and the caller retains its bounded activation predicate.
        if (! activationLifecycleEventsAllowed)
        {
            if (millisecondsToRunFor > 0)
                CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                                   static_cast<CFTimeInterval>(millisecondsToRunFor) * 0.001, true);
            return;
        }

        constexpr auto lifecycleEventMask = NSEventMaskAppKitDefined
                                          | NSEventMaskApplicationDefined;
        if (NSEvent* event = [NSApp nextEventMatchingMask:lifecycleEventMask
                                                 untilDate:[NSDate distantPast]
                                                    inMode:NSDefaultRunLoopMode
                                                   dequeue:YES])
        {
            [NSApp sendEvent:event];
            return;
        }

        if (millisecondsToRunFor > 0)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode,
                               static_cast<CFTimeInterval>(millisecondsToRunFor) * 0.001, true);
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
            {
                activationLifecycleEventsAllowed = false;
                return std::unique_ptr<NativeFilePanel>(new NativeFilePanel(candidate));
            }
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
