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
    static std::unique_ptr<NativeFilePanel> findVisible(bool importing, const char* title);
    static int visibleCount();
    ~NativeFilePanel();
    NativeFilePanel(const NativeFilePanel&) = delete;
    NativeFilePanel& operator=(const NativeFilePanel&) = delete;
    bool isAlive() const;
    bool isVisible() const;
    bool hasDelegate() const;
    bool beganExactlyOnce() const;
    bool completionHasNotStarted() const;
    bool completionProgressIsValid() const;
    // A native completion is terminal only after AppKit released its block:
    // either the callback returned once, or the block was never entered.
    bool completionResolvedExactlyOnce() const;
    static bool hasActiveCompletionSession();
    std::string className() const;
    void useFixtureLocation(const std::string& directory, const std::string& filename);
private:
    NativeFilePanel(void*, void*);
    void* panel = nullptr;
    void* observation = nullptr;
};
