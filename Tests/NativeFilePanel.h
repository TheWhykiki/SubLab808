#pragma once
#include <cstddef>
#include <memory>
#include <string>

// Test-only observer for panels owned by this process. The opaque identity is
// re-resolved through NSApp's live windows; no panel is retained or dereferenced
// after removal, and no other app is inspected.
class NativeFilePanel final
{
public:
    using ApplicationStopCallback = bool (*)() noexcept;
    // Must run before ScopedJuceInitialiser_GUI creates NSApplication.
    static void installTestApplication();
    static void prepareTestApplication(ApplicationStopCallback);
    [[nodiscard]] static bool applicationIsRunning() noexcept;
    // Private START/BARRIER/SETTLE/STOP events prove distinct dispatches through
    // the real NSApplication event loop. BARRIER plus CFRunLoopStop returns the
    // active fetch; only its return path queues SETTLE for a later fetch. The
    // test never pumps or sends an event itself.
    [[nodiscard]] static bool postApplicationStartEvent() noexcept;
    [[nodiscard]] static bool applicationStartEventWasHandled() noexcept;
    [[nodiscard]] static bool applicationIsReadyForSettleEvent() noexcept;
    [[nodiscard]] static bool postApplicationSettleEvent() noexcept;
    [[nodiscard]] static bool requestApplicationEventFetchReturn() noexcept;
    [[nodiscard]] static bool applicationSettleEventWasHandled() noexcept;
    [[nodiscard]] static bool applicationIsReadyForStopEvent() noexcept;
    [[nodiscard]] static bool postApplicationStopEvent() noexcept;
    [[nodiscard]] static bool applicationStopEventWasPosted() noexcept;
    [[nodiscard]] static bool applicationStopEventWasHandled() noexcept;
    static void finishTestApplication() noexcept;
    // A captured, test-owned JUCE desktop peer must not leave an AppKit
    // display-link animation running after the console test exits.
    static void disableAutomaticWindowAnimations(void* nativeView);
    static std::unique_ptr<NativeFilePanel> findVisible(bool importing, const char* title);
    static int visibleCount();
    ~NativeFilePanel();
    NativeFilePanel(const NativeFilePanel&) = delete;
    NativeFilePanel& operator=(const NativeFilePanel&) = delete;
    bool isAlive() const;
    bool isVisible() const;
    bool hasDelegate() const;
    bool beganExactlyOnce() const;
    bool moduleWasRetainedAtBegin() const;
    bool completionHasNotStarted() const;
    bool completionProgressIsValid() const;
    bool completionIsQuiescent() const;
    // Call only after JUCE destroyed its native-modal component and AppKit
    // removed the closed panel/delegate. Idempotent for cleanup/fence checks.
    bool markSafeOwnerRetired();
    std::size_t lateCompletionEntryCount() const;
    static std::size_t totalLateCompletionEntryCount();
    static bool hasActiveCompletionSession();
    std::string className() const;
    void useFixtureLocation(const std::string& directory, const std::string& filename);
private:
    NativeFilePanel(void*, void*);
    void* panel = nullptr;
    void* observation = nullptr;
};
