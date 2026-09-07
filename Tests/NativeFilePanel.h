#pragma once
#include <memory>
#include <string>

// Test-only observer for panels owned by this process. The opaque identity is
// re-resolved through NSApp's live windows; no panel is retained or dereferenced
// after removal, and no other app is inspected.
class NativeFilePanel final
{
public:
    static void prepareTestApplication();
    // The short-lived synthetic host window must not leave an AppKit display-
    // link animation running after the console test exits.
    static void disableAutomaticHostWindowAnimations(void* nativeView);
    // Dispatch the default AppKit/JUCE run-loop mode without re-entering
    // NSApplication's unbounded top-level run loop.
    static void dispatchEventsFor(int millisecondsToRunFor);
    static std::unique_ptr<NativeFilePanel> findVisible(bool importing, const char* title);
    static int visibleCount();
    ~NativeFilePanel();
    NativeFilePanel(const NativeFilePanel&) = delete;
    NativeFilePanel& operator=(const NativeFilePanel&) = delete;
    bool isAlive() const;
    bool isVisible() const;
    bool hasDelegate() const;
    std::string className() const;
    void useFixtureLocation(const std::string& directory, const std::string& filename);
private:
    explicit NativeFilePanel(void*);
    void* panel = nullptr;
};
