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
}

NativeFilePanel::NativeFilePanel(void* nativePanel) : panel(nativePanel)
{
    [(NSSavePanel*) panel retain];
}
NativeFilePanel::~NativeFilePanel()
{
    [(NSSavePanel*) panel release];
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
bool NativeFilePanel::isVisible() const { return [(NSSavePanel*) panel isVisible]; }
bool NativeFilePanel::hasDelegate() const { return [(NSSavePanel*) panel delegate] != nil; }
std::string NativeFilePanel::className() const
{
    @autoreleasepool { return NSStringFromClass([(NSSavePanel*) panel class]).UTF8String; }
}
void NativeFilePanel::useFixtureLocation(const std::string& directory, const std::string& filename)
{
    @autoreleasepool
    {
        auto* candidate = (NSSavePanel*) panel;
        [candidate setDirectoryURL:[NSURL fileURLWithPath:checkedUTF8(directory.c_str())
                                             isDirectory:YES]];
        [candidate setNameFieldStringValue:checkedUTF8(filename.c_str())];
    }
}
