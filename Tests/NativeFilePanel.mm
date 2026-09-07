#include "NativeFilePanel.h"
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
    // regular app. The first controlled NSApplication run slice completes the
    // launch sequence exactly once; calling finishLaunching here would make
    // AppKit send its launch notifications again when run starts.
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular)
            throw std::runtime_error("NATIVE_PANEL_SETUP: test application cannot become a regular GUI process");
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
void NativeFilePanel::runApplicationLoopFor(int millisecondsToRunFor)
{
    if (millisecondsToRunFor < 0)
        throw std::runtime_error("Native panel event loop received a negative duration");

    @autoreleasepool
    {
        if (![NSThread isMainThread] || [NSApp isRunning])
            throw std::runtime_error("Native panel tests require a non-nested main NSApplication loop");

        // A real NSApplication loop, rather than a private CFRunLoop/sendEvent
        // approximation, lets AppKit retire one panel session before the next
        // starts. stop: called by a timer needs an NSEvent to wake the loop, as
        // documented by AppKit. Invalidating the timer after run returns also
        // prevents a prematurely ended slice from stopping a later slice.
        __block bool stopRequested = false;
        auto* stopTimer = [NSTimer timerWithTimeInterval:millisecondsToRunFor * 0.001
                                                 repeats:NO
                                                   block:^(NSTimer*) {
            stopRequested = true;
            [NSApp stop:nil];
            auto* wakeEvent = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                  location:NSZeroPoint
                                             modifierFlags:0
                                                 timestamp:0
                                              windowNumber:0
                                                   context:nil
                                                   subtype:0
                                                     data1:0
                                                     data2:0];
            [NSApp postEvent:wakeEvent atStart:YES];
        }];
        [[NSRunLoop mainRunLoop] addTimer:stopTimer forMode:NSRunLoopCommonModes];
        [NSApp run];
        [stopTimer invalidate];
        if (!stopRequested)
            throw std::runtime_error("Native panel NSApplication loop stopped outside its test slice");
        if ([NSApp isRunning])
            throw std::runtime_error("Native panel NSApplication loop remained active after its test slice");
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
