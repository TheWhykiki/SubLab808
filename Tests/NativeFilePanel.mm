#include "NativeFilePanel.h"
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
    // ScopedJuceInitialiser_GUI in a console test does not run NSApplication's
    // normal launch sequence. Native panels need a genuine activatable app.
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular)
            throw std::runtime_error("NATIVE_PANEL_SETUP: test application cannot become a regular GUI process");
        [NSApp finishLaunching];
    }
}
void NativeFilePanel::dispatchEventsFor(int millisecondsToRunFor)
{
    if (millisecondsToRunFor < 0)
        throw std::runtime_error("Native panel event dispatch received a negative duration");

    // This mirrors JUCE's macOS runDispatchLoopUntil() closely, except that
    // AppKit event retrieval is deliberately non-blocking. JUCE 8.0.15 asks
    // nextEventMatchingMask() to wait until a date in the future, and JUCE issue
    // #1574 documents delayed event delivery around that boundary. CI observed
    // dispatch calls outliving the surrounding lifecycle deadlines. distantPast
    // removes that avoidable wait and only dequeues an event already available;
    // CTest remains the hard watchdog for code executed by an event callback.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(millisecondsToRunFor);
    while (std::chrono::steady_clock::now() < deadline)
    {
        @autoreleasepool
        {
            const auto remaining = std::chrono::duration<double>(deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0.0) break;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, remaining < 0.001 ? remaining : 0.001, true);

            if (NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                     untilDate:[NSDate distantPast]
                                                        inMode:NSDefaultRunLoopMode
                                                       dequeue:YES])
                [NSApp sendEvent:event];
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
