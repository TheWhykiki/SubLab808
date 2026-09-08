#include "NativeFilePanel.h"
#include "MacModulePin.h"
#include <cstdio>
#include <stdexcept>
#import <AppKit/AppKit.h>
#import <objc/runtime.h>

@interface WhyKikiNativePanelSessionObservation : NSObject
{
@public
    NSUInteger beginCount;
    NSUInteger beginReturnCount;
    NSUInteger completionEntryCount;
    NSUInteger completionReturnCount;
    NSUInteger completionDiscardCount;
    NSUInteger completionBlockReleaseCount;
    bool moduleRetainedAtBegin;
    bool safeOwnerRetired;
    NSUInteger lateCompletionEntryCount;
}
@end

@implementation WhyKikiNativePanelSessionObservation
@end

@interface WhyKikiNativePanelCompletionLifetime : NSObject
{
    WhyKikiNativePanelSessionObservation* observation;
}
- (instancetype)initWithObservation:(WhyKikiNativePanelSessionObservation*)state;
- (void)completionWillEnter;
- (void)completionDidReturn;
@end

namespace
{
using CompletionHandler = void (^)(NSModalResponse);
using BeginWithCompletionHandler = void (*)(id, SEL, CompletionHandler);

BeginWithCompletionHandler originalBeginWithCompletionHandler = nullptr;
NSMutableSet* activeCompletionObservations = nil;
id applicationEventMonitor = nil;
NativeFilePanel::ApplicationStopCallback applicationStopCallback = nullptr;
NSUInteger applicationStartEventCount = 0;
NSUInteger applicationSettleEventCount = 0;
NSUInteger applicationStopEventCount = 0;
bool applicationStartEventHandledWhileRunning = false;
bool applicationSettleEventHandledWhileRunning = false;
bool applicationSettleEventPostedFromReadyContext = false;
bool applicationSettleEventWasCurrentEvent = false;
bool applicationSettleEventHandledWithoutModalWindow = false;
bool applicationStopEventHandledWhileRunning = false;
bool applicationStopEventPostedFromReadyContext = false;
bool applicationStopEventWasCurrentEvent = false;
bool applicationStopEventHandledWithoutModalWindow = false;
bool applicationStopCallbackSucceeded = false;
bool applicationStartEventPosted = false;
bool applicationSettleEventPosted = false;
bool applicationStopEventPosted = false;
NSUInteger globalLateCompletionEntryCount = 0;
char completionObservationKey;

constexpr short applicationControlEventSubtype = 0x574b;
constexpr NSInteger applicationControlEventMagic = 0x5748594b;
constexpr NSInteger applicationStartEventCode = 1;
constexpr NSInteger applicationSettleEventCode = 2;
constexpr NSInteger applicationStopEventCode = 3;

bool isRunnableApplicationEventContext() noexcept
{
    return [NSThread isMainThread] && NSApp != nil && [NSApp isRunning]
        && [NSApp modalWindow] == nil;
}

bool postApplicationControlEvent(NSInteger code) noexcept
{
    @autoreleasepool
    {
        if (![NSThread isMainThread] || NSApp == nil || applicationEventMonitor == nil)
            return false;

        auto* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                         location:NSZeroPoint
                                    modifierFlags:0
                                        timestamp:0.0
                                     windowNumber:0
                                          context:nil
                                          subtype:applicationControlEventSubtype
                                            data1:applicationControlEventMagic
                                            data2:code];
        if (event == nil)
            return false;
        // JUCE's common-mode CFRunLoop source may remain continuously ready
        // while the shutdown timer is armed, so AppKit does not guarantee
        // progress for a private event left at the queue tail. Front insertion
        // is still asynchronous; the local monitor below must observe this
        // exact event through NSApplication's real dispatch path.
        [NSApp postEvent:event atStart:YES];
        return true;
    }
}

bool resolvedExactlyOnce(const WhyKikiNativePanelSessionObservation* state)
{
    if (state == nil || state->beginCount != 1 || state->beginReturnCount != 1)
        return false;

    const auto releasedAfterReturn = state->completionEntryCount == 1
                                  && state->completionReturnCount == 1
                                  && state->completionDiscardCount == 0
                                  && state->completionBlockReleaseCount == 1;
    const auto releasedWithoutEntry = state->completionEntryCount == 0
                                   && state->completionReturnCount == 0
                                   && state->completionDiscardCount == 1
                                   && state->completionBlockReleaseCount == 1;
    return releasedAfterReturn || releasedWithoutEntry;
}

bool completionProgressIsValid(const WhyKikiNativePanelSessionObservation* state)
{
    if (state == nil || state->beginCount != 1 || state->beginReturnCount != 1)
        return false;

    return state->completionEntryCount <= 1
        && state->completionReturnCount <= state->completionEntryCount
        && state->completionDiscardCount <= 1
        && state->completionBlockReleaseCount <= 1
        && state->completionDiscardCount <= state->completionBlockReleaseCount
        && (state->completionDiscardCount == 0 || state->completionEntryCount == 0)
        && (state->completionBlockReleaseCount == 0 || resolvedExactlyOnce(state))
        && state->lateCompletionEntryCount <= state->completionEntryCount
        && (state->lateCompletionEntryCount == 0 || state->safeOwnerRetired);
}

bool completionIsQuiescent(const WhyKikiNativePanelSessionObservation* state)
{
    return completionProgressIsValid(state)
        && state->completionReturnCount == state->completionEntryCount;
}

void retireObservationIfSafe(WhyKikiNativePanelSessionObservation* state)
{
    if (resolvedExactlyOnce(state)
        || (state != nil && state->safeOwnerRetired && completionIsQuiescent(state)))
        [activeCompletionObservations removeObject:state];
}

bool markSafeOwnerRetired(WhyKikiNativePanelSessionObservation* state)
{
    if (! completionIsQuiescent(state)) return false;
    state->safeOwnerRetired = true;
    retireObservationIfSafe(state);
    return true;
}

}

@implementation WhyKikiNativePanelCompletionLifetime
- (instancetype)initWithObservation:(WhyKikiNativePanelSessionObservation*)state
{
    self = [super init];
    if (self != nil) observation = [state retain];
    return self;
}
- (void)dealloc
{
    @synchronized ([NSSavePanel class])
    {
        // Block release is diagnostic only. AppKit does not promise to release
        // a modeless completion immediately after a programmatic panel close.
        if (observation->completionEntryCount == 0
            && observation->completionReturnCount == 0)
            ++observation->completionDiscardCount;
        ++observation->completionBlockReleaseCount;
        retireObservationIfSafe(observation);
    }
    [observation release];
    [super dealloc];
}
- (void)completionWillEnter
{
    @synchronized ([NSSavePanel class])
    {
        if (observation->safeOwnerRetired)
        {
            ++observation->lateCompletionEntryCount;
            ++globalLateCompletionEntryCount;
            [activeCompletionObservations addObject:observation];
        }
        ++observation->completionEntryCount;
    }
}
- (void)completionDidReturn
{
    @synchronized ([NSSavePanel class])
    {
        ++observation->completionReturnCount;
        retireObservationIfSafe(observation);
    }
}
@end

namespace
{

class ScopedCompletionLifetime final
{
public:
    explicit ScopedCompletionLifetime(WhyKikiNativePanelCompletionLifetime* value)
        : lifetime([value retain]) {}
    ~ScopedCompletionLifetime() { [lifetime release]; }
    WhyKikiNativePanelCompletionLifetime* get() const { return lifetime; }
private:
    WhyKikiNativePanelCompletionLifetime* lifetime;
};

void verifyCompletionObserverContract()
{
    const auto lateEntryBaseline = globalLateCompletionEntryCount;
    auto* returnedState = [[WhyKikiNativePanelSessionObservation alloc] init];
    returnedState->beginCount = 1;
    returnedState->beginReturnCount = 1;
    auto* returnedLifetime = [[WhyKikiNativePanelCompletionLifetime alloc]
        initWithObservation:returnedState];
    [activeCompletionObservations addObject:returnedState];
    [returnedLifetime completionWillEnter];
    [returnedLifetime completionDidReturn];
    if (! completionIsQuiescent(returnedState)
        || ! markSafeOwnerRetired(returnedState)
        || [activeCompletionObservations containsObject:returnedState])
        throw std::runtime_error("NATIVE_PANEL_SETUP: returned completion did not reach safe owner retirement");
    [returnedLifetime release];
    if (! completionProgressIsValid(returnedState))
        throw std::runtime_error("NATIVE_PANEL_SETUP: returned completion release accounting failed");
    [returnedState release];

    auto* discardedState = [[WhyKikiNativePanelSessionObservation alloc] init];
    discardedState->beginCount = 1;
    discardedState->beginReturnCount = 1;
    auto* discardedLifetime = [[WhyKikiNativePanelCompletionLifetime alloc]
        initWithObservation:discardedState];
    [activeCompletionObservations addObject:discardedState];
    if (! completionIsQuiescent(discardedState)
        || ! markSafeOwnerRetired(discardedState)
        || [activeCompletionObservations containsObject:discardedState])
        throw std::runtime_error("NATIVE_PANEL_SETUP: unentered completion did not reach safe owner retirement");
    [discardedLifetime release];
    if (! completionProgressIsValid(discardedState))
        throw std::runtime_error("NATIVE_PANEL_SETUP: unentered completion release accounting failed");
    [discardedState release];

    auto* lateState = [[WhyKikiNativePanelSessionObservation alloc] init];
    lateState->beginCount = 1;
    lateState->beginReturnCount = 1;
    auto* lateLifetime = [[WhyKikiNativePanelCompletionLifetime alloc]
        initWithObservation:lateState];
    [activeCompletionObservations addObject:lateState];
    if (! markSafeOwnerRetired(lateState)
        || [activeCompletionObservations containsObject:lateState])
        throw std::runtime_error("NATIVE_PANEL_SETUP: late-completion fixture did not retire its owner");
    [lateLifetime completionWillEnter];
    if (lateState->lateCompletionEntryCount != 1
        || globalLateCompletionEntryCount != lateEntryBaseline + 1
        || ! [activeCompletionObservations containsObject:lateState])
        throw std::runtime_error("NATIVE_PANEL_SETUP: late completion was not reactivated and counted");
    [lateLifetime completionDidReturn];
    if (! completionIsQuiescent(lateState)
        || [activeCompletionObservations containsObject:lateState])
        throw std::runtime_error("NATIVE_PANEL_SETUP: late completion did not return to quiescence");
    [lateLifetime release];
    if (! completionProgressIsValid(lateState))
        throw std::runtime_error("NATIVE_PANEL_SETUP: late completion release accounting failed");
    globalLateCompletionEntryCount = lateEntryBaseline;
    [lateState release];
}

void trackedBeginWithCompletionHandler(id self, SEL selector, CompletionHandler handler)
{
    if (handler == nil)
    {
        originalBeginWithCompletionHandler(self, selector, handler);
        return;
    }

    // This short-lived console harness exits immediately after its final
    // lifecycle audit. Prevent AppKit's private window-transform display link
    // from outliving that process teardown; the real panel, delegate and
    // completion path remain unchanged and fully observed.
    [(NSSavePanel*) self setAnimationBehavior:NSWindowAnimationBehaviorNone];

    WhyKikiNativePanelSessionObservation* state = nil;
    @synchronized ([NSSavePanel class])
    {
        state = (WhyKikiNativePanelSessionObservation*)
            objc_getAssociatedObject(self, &completionObservationKey);
        if (state == nil || state->safeOwnerRetired || resolvedExactlyOnce(state))
        {
            state = [[WhyKikiNativePanelSessionObservation alloc] init];
            objc_setAssociatedObject(self, &completionObservationKey, state,
                                     OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            [state release];
        }
        [state retain];
        state->moduleRetainedAtBegin = wk::currentModuleIsRetainedForNativeCallbacks();
        ++state->beginCount;
        [activeCompletionObservations addObject:state];
    }

    WhyKikiNativePanelCompletionLifetime* lifetime =
        [[WhyKikiNativePanelCompletionLifetime alloc] initWithObservation:state];
    CompletionHandler trackedHandler = ^(NSModalResponse result)
    {
        // The supplied handler may synchronously tear down the panel and its
        // last AppKit block owner. Keep the sentinel alive through the return
        // accounting even if that happens reentrantly.
        ScopedCompletionLifetime executing(lifetime);
        [executing.get() completionWillEnter];
        handler(result);
        [executing.get() completionDidReturn];
    };
    originalBeginWithCompletionHandler(self, selector, trackedHandler);
    @synchronized ([NSSavePanel class])
    {
        ++state->beginReturnCount;
        retireObservationIfSafe(state);
    }
    [lifetime release];
    [state release];
}

NSString* checkedUTF8(const char* text)
{
    auto* result = [NSString stringWithUTF8String:text];
    if (result == nil) throw std::runtime_error("Native panel test received invalid UTF-8");
    return result;
}

NSSavePanel* resolvePanel(void* identity, void* expectedObservation)
{
    // JUCE uses releasedWhenClosed for these panels, so retaining one here
    // would change the lifecycle that this bridge is meant to observe.
    for (NSWindow* window in [NSApp windows])
        if ((void*) window == identity && [window isKindOfClass:[NSSavePanel class]]
            && (expectedObservation == nullptr
                || objc_getAssociatedObject(window, &completionObservationKey) == (id) expectedObservation))
            return (NSSavePanel*) window;
    return nil;
}
}

NativeFilePanel::NativeFilePanel(void* nativePanel, void* observationToRetain)
    : panel(nativePanel), observation([(id) observationToRetain retain]) {}
NativeFilePanel::~NativeFilePanel()
{
    [(id) observation release];
}
bool NativeFilePanel::isAlive() const
{
    @autoreleasepool
    {
        return resolvePanel(panel, observation) != nil;
    }
}
void NativeFilePanel::prepareTestApplication(ApplicationStopCallback stopCallback)
{
    // ScopedJuceInitialiser_GUI in a console test does not provide an active
    // regular app. Create and configure one here; the coordinator independently
    // waits for the top-level NSApplication event loop before touching the UI.
    @autoreleasepool
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("NATIVE_PANEL_SETUP: native chooser tests require the main thread");
        if (stopCallback == nullptr)
            throw std::runtime_error("NATIVE_PANEL_SETUP: application stop callback is missing");
        [NSApplication sharedApplication];
        if ([NSApp isRunning])
            throw std::runtime_error("NATIVE_PANEL_SETUP: event monitor must be installed before the app loop");
        if (applicationEventMonitor != nil)
            throw std::runtime_error("NATIVE_PANEL_SETUP: application event monitor is already active");

        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if ([NSApp activationPolicy] != NSApplicationActivationPolicyRegular)
            throw std::runtime_error("NATIVE_PANEL_SETUP: test application cannot become a regular GUI process");

        if (originalBeginWithCompletionHandler == nullptr)
        {
            if (activeCompletionObservations == nil)
            {
                activeCompletionObservations = [[NSMutableSet alloc] init];
                verifyCompletionObserverContract();
            }
            auto saveMethod = class_getInstanceMethod([NSSavePanel class],
                                                      @selector(beginWithCompletionHandler:));
            auto openMethod = class_getInstanceMethod([NSOpenPanel class],
                                                      @selector(beginWithCompletionHandler:));
            if (saveMethod == nullptr || openMethod == nullptr || saveMethod != openMethod)
                throw std::runtime_error("NATIVE_PANEL_SETUP: cannot observe native panel completion");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullable-to-nonnull-conversion"
            auto original = method_getImplementation(saveMethod);
#pragma clang diagnostic pop
            const auto replacement = reinterpret_cast<IMP>(trackedBeginWithCompletionHandler);
            if (original == nullptr || original == replacement)
                throw std::runtime_error("NATIVE_PANEL_SETUP: cannot install native panel completion observer");
            originalBeginWithCompletionHandler = reinterpret_cast<BeginWithCompletionHandler>(original);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullable-to-nonnull-conversion"
            auto displaced = method_setImplementation(saveMethod, replacement);
#pragma clang diagnostic pop
            if (displaced != original)
                throw std::runtime_error("NATIVE_PANEL_SETUP: native panel completion observer changed concurrently");
        }

        // Install last, after every other setup operation that can throw. The
        // caller's scope guard removes this monitor exactly once after the loop.
        applicationStopCallback = stopCallback;
        applicationStartEventCount = 0;
        applicationSettleEventCount = 0;
        applicationStopEventCount = 0;
        applicationStartEventHandledWhileRunning = false;
        applicationSettleEventHandledWhileRunning = false;
        applicationSettleEventPostedFromReadyContext = false;
        applicationSettleEventWasCurrentEvent = false;
        applicationSettleEventHandledWithoutModalWindow = false;
        applicationStopEventHandledWhileRunning = false;
        applicationStopEventPostedFromReadyContext = false;
        applicationStopEventWasCurrentEvent = false;
        applicationStopEventHandledWithoutModalWindow = false;
        applicationStopCallbackSucceeded = false;
        applicationStartEventPosted = false;
        applicationSettleEventPosted = false;
        applicationStopEventPosted = false;
        applicationEventMonitor = [NSEvent
            addLocalMonitorForEventsMatchingMask:NSEventMaskApplicationDefined
                                       handler:^NSEvent* (NSEvent* event)
                                       {
                                           if ([event type] != NSEventTypeApplicationDefined
                                               || [event subtype] != applicationControlEventSubtype
                                               || [event data1] != applicationControlEventMagic)
                                               return event;

                                           const bool running = [NSApp isRunning];
                                           if ([event data2] == applicationStartEventCode)
                                           {
                                               ++applicationStartEventCount;
                                               applicationStartEventHandledWhileRunning = running;
                                               std::fprintf(stderr,
                                                            "NATIVE_APP_LOOP_START_HANDLED count=%lu running=%d\n",
                                                            static_cast<unsigned long>(applicationStartEventCount),
                                                            running ? 1 : 0);
                                               std::fflush(stderr);
                                               // START is a private dispatch barrier, not app input.
                                               return nil;
                                           }
                                           if ([event data2] == applicationSettleEventCode)
                                           {
                                               ++applicationSettleEventCount;
                                               applicationSettleEventHandledWhileRunning = running;
                                               applicationSettleEventWasCurrentEvent =
                                                   [NSApp currentEvent] == event;
                                               applicationSettleEventHandledWithoutModalWindow =
                                                   [NSApp modalWindow] == nil;
                                               std::fprintf(stderr,
                                                            "NATIVE_APP_LOOP_SETTLE_HANDLED count=%lu running=%d postedFromReady=%d currentEvent=%d modalWindow=%d\n",
                                                            static_cast<unsigned long>(applicationSettleEventCount),
                                                            running ? 1 : 0,
                                                            applicationSettleEventPostedFromReadyContext ? 1 : 0,
                                                            applicationSettleEventWasCurrentEvent ? 1 : 0,
                                                            applicationSettleEventHandledWithoutModalWindow ? 0 : 1);
                                               std::fflush(stderr);
                                               // The coordinator must observe this completed dispatch
                                               // before it may enqueue STOP on a later timer turn.
                                               return nil;
                                           }
                                           else if ([event data2] == applicationStopEventCode)
                                           {
                                               ++applicationStopEventCount;
                                               applicationStopEventHandledWhileRunning = running;
                                               applicationStopEventWasCurrentEvent =
                                                   [NSApp currentEvent] == event;
                                               applicationStopEventHandledWithoutModalWindow =
                                                   [NSApp modalWindow] == nil;
                                               applicationStopCallbackSucceeded = running
                                                   && applicationStopEventPostedFromReadyContext
                                                   && applicationStopEventWasCurrentEvent
                                                   && applicationStopEventHandledWithoutModalWindow
                                                   && applicationStopCallback != nullptr
                                                   && applicationStopCallback();
                                               std::fprintf(stderr,
                                                            "NATIVE_APP_LOOP_STOP_HANDLED count=%lu running=%d postedFromReady=%d currentEvent=%d modalWindow=%d callback=%d\n",
                                                            static_cast<unsigned long>(applicationStopEventCount),
                                                            running ? 1 : 0,
                                                            applicationStopEventPostedFromReadyContext ? 1 : 0,
                                                            applicationStopEventWasCurrentEvent ? 1 : 0,
                                                            applicationStopEventHandledWithoutModalWindow ? 0 : 1,
                                                            applicationStopCallbackSucceeded ? 1 : 0);
                                               std::fflush(stderr);
                                               // STOP is private control input and has already
                                               // provided the required real event-handler boundary.
                                               // Do not send it through NSApplication after stop:.
                                               return nil;
                                           }
                                           std::fflush(stderr);
                                           return event;
                                       }];
        if (applicationEventMonitor == nil)
        {
            applicationStopCallback = nullptr;
            throw std::runtime_error("NATIVE_PANEL_SETUP: cannot install application event monitor");
        }
    }
}
bool NativeFilePanel::applicationIsRunning() noexcept
{
    @autoreleasepool
    {
        return [NSThread isMainThread] && NSApp != nil && [NSApp isRunning];
    }
}
bool NativeFilePanel::postApplicationStartEvent() noexcept
{
    @autoreleasepool
    {
        if (![NSThread isMainThread] || applicationStartEventPosted
            || applicationStartEventCount != 0)
            return false;
        applicationStartEventPosted = postApplicationControlEvent(applicationStartEventCode);
        return applicationStartEventPosted;
    }
}
bool NativeFilePanel::applicationStartEventWasHandled() noexcept
{
    @autoreleasepool
    {
        return [NSThread isMainThread] && applicationStartEventPosted
            && applicationStartEventCount == 1
            && applicationStartEventHandledWhileRunning;
    }
}
bool NativeFilePanel::applicationIsReadyForSettleEvent() noexcept
{
    @autoreleasepool
    {
        return isRunnableApplicationEventContext();
    }
}
bool NativeFilePanel::postApplicationSettleEvent() noexcept
{
    @autoreleasepool
    {
        const auto postedFromReadyContext = applicationIsReadyForSettleEvent();
        if (! postedFromReadyContext || applicationSettleEventPosted
            || applicationSettleEventCount != 0 || applicationStopEventPosted
            || applicationStopEventCount != 0)
            return false;
        applicationSettleEventPosted = postApplicationControlEvent(applicationSettleEventCode);
        applicationSettleEventPostedFromReadyContext =
            postedFromReadyContext && applicationSettleEventPosted;
        return applicationSettleEventPosted;
    }
}
bool NativeFilePanel::applicationSettleEventWasHandled() noexcept
{
    @autoreleasepool
    {
        return [NSThread isMainThread] && applicationSettleEventPosted
            && applicationSettleEventCount == 1
            && applicationSettleEventHandledWhileRunning
            && applicationSettleEventPostedFromReadyContext
            && applicationSettleEventWasCurrentEvent
            && applicationSettleEventHandledWithoutModalWindow;
    }
}
bool NativeFilePanel::applicationIsReadyForStopEvent() noexcept
{
    @autoreleasepool
    {
        return applicationSettleEventWasHandled()
            && isRunnableApplicationEventContext();
    }
}
bool NativeFilePanel::postApplicationStopEvent() noexcept
{
    @autoreleasepool
    {
        const auto postedFromReadyContext = applicationIsReadyForStopEvent();
        if (! postedFromReadyContext || applicationStopEventPosted
            || applicationStopEventCount != 0)
            return false;
        applicationStopEventPosted = postApplicationControlEvent(applicationStopEventCode);
        applicationStopEventPostedFromReadyContext =
            postedFromReadyContext && applicationStopEventPosted;
        return applicationStopEventPosted;
    }
}
bool NativeFilePanel::applicationStopEventWasHandled() noexcept
{
    @autoreleasepool
    {
        return [NSThread isMainThread] && applicationStopEventPosted
            && applicationStopEventCount == 1
            && applicationStopEventHandledWhileRunning
            && applicationStopEventPostedFromReadyContext
            && applicationStopEventWasCurrentEvent
            && applicationStopEventHandledWithoutModalWindow
            && applicationStopCallbackSucceeded;
    }
}
void NativeFilePanel::finishTestApplication() noexcept
{
    @autoreleasepool
    {
        if (![NSThread isMainThread] || applicationEventMonitor == nil)
            return;
        // JUCE's macOS stopDispatchLoop() starts AppKit periodic events to wake
        // NSApplication::run. This dedicated console process must retire that
        // wake source after run has returned, before framework/global teardown.
        [NSEvent stopPeriodicEvents];
        [NSEvent removeMonitor:applicationEventMonitor];
        applicationEventMonitor = nil;
        applicationStopCallback = nullptr;
    }
}
void NativeFilePanel::disableAutomaticWindowAnimations(void* nativeView)
{
    @autoreleasepool
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("Native panel tests require AppKit window access on the main thread");
        auto* view = (NSView*) nativeView;
        NSWindow* window = view == nil || ! [view isKindOfClass:[NSView class]] ? nil : [view window];
        if (window == nil)
            throw std::runtime_error("Native panel tests could not resolve a captured JUCE peer window");
        [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
        if ([window animationBehavior] != NSWindowAnimationBehaviorNone)
            throw std::runtime_error("Native panel tests could not disable automatic AppKit window animations");
    }
}
std::unique_ptr<NativeFilePanel> NativeFilePanel::findVisible(bool importing, const char* title)
{
    @autoreleasepool
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("NATIVE_PANEL_OBSERVER: panel inspection requires the main thread");
        for (NSWindow* window in [NSApp windows])
        {
            if (![window isKindOfClass:[NSSavePanel class]] || ![window isVisible])
                continue;
            auto* candidate = (NSSavePanel*) window;
            const bool isOpen = [candidate isKindOfClass:[NSOpenPanel class]];
            if (isOpen == importing && [[candidate title] isEqualToString:checkedUTF8(title)])
            {
                id tracked = objc_getAssociatedObject(candidate, &completionObservationKey);
                if (tracked == nil)
                    throw std::runtime_error("NATIVE_PANEL_OBSERVER: visible panel has no tracked completion session");
                return std::unique_ptr<NativeFilePanel>(new NativeFilePanel(candidate, (void*) tracked));
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
        auto* candidate = resolvePanel(panel, observation);
        return candidate != nil && [candidate isVisible];
    }
}
bool NativeFilePanel::hasDelegate() const
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel, observation);
        return candidate != nil && [candidate delegate] != nil;
    }
}
bool NativeFilePanel::beganExactlyOnce() const
{
    @synchronized ([NSSavePanel class])
    {
        auto* state = (WhyKikiNativePanelSessionObservation*) observation;
        return state != nil && state->beginCount == 1 && state->beginReturnCount == 1;
    }
}
bool NativeFilePanel::moduleWasRetainedAtBegin() const
{
    @synchronized ([NSSavePanel class])
    {
        auto* state = (WhyKikiNativePanelSessionObservation*) observation;
        return state != nil && state->moduleRetainedAtBegin;
    }
}
bool NativeFilePanel::completionHasNotStarted() const
{
    @synchronized ([NSSavePanel class])
    {
        auto* state = (WhyKikiNativePanelSessionObservation*) observation;
        return state != nil && state->beginCount == 1 && state->beginReturnCount == 1
            && state->completionEntryCount == 0 && state->completionReturnCount == 0
            && state->completionDiscardCount == 0
            && state->completionBlockReleaseCount == 0
            && ! state->safeOwnerRetired && state->lateCompletionEntryCount == 0;
    }
}
bool NativeFilePanel::completionProgressIsValid() const
{
    @synchronized ([NSSavePanel class])
    {
        return ::completionProgressIsValid(
            (WhyKikiNativePanelSessionObservation*) observation);
    }
}
bool NativeFilePanel::completionIsQuiescent() const
{
    @synchronized ([NSSavePanel class])
    {
        return ::completionIsQuiescent(
            (WhyKikiNativePanelSessionObservation*) observation);
    }
}
bool NativeFilePanel::markSafeOwnerRetired()
{
    @synchronized ([NSSavePanel class])
    {
        return ::markSafeOwnerRetired(
            (WhyKikiNativePanelSessionObservation*) observation);
    }
}
std::size_t NativeFilePanel::lateCompletionEntryCount() const
{
    @synchronized ([NSSavePanel class])
    {
        auto* state = (WhyKikiNativePanelSessionObservation*) observation;
        return state == nil ? 0 : static_cast<std::size_t>(state->lateCompletionEntryCount);
    }
}
std::size_t NativeFilePanel::totalLateCompletionEntryCount()
{
    @synchronized ([NSSavePanel class])
    {
        return static_cast<std::size_t>(globalLateCompletionEntryCount);
    }
}
bool NativeFilePanel::hasActiveCompletionSession()
{
    @synchronized ([NSSavePanel class])
    {
        return activeCompletionObservations != nil
            && [activeCompletionObservations count] != 0;
    }
}
std::string NativeFilePanel::className() const
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel, observation);
        if (candidate == nil) throw std::runtime_error("Native panel disappeared before inspection");
        return NSStringFromClass([candidate class]).UTF8String;
    }
}
void NativeFilePanel::useFixtureLocation(const std::string& directory, const std::string& filename)
{
    @autoreleasepool
    {
        auto* candidate = resolvePanel(panel, observation);
        if (candidate == nil) throw std::runtime_error("Native panel disappeared before fixture setup");
        [candidate setDirectoryURL:[NSURL fileURLWithPath:checkedUTF8(directory.c_str())
                                             isDirectory:YES]];
        [candidate setNameFieldStringValue:checkedUTF8(filename.c_str())];
    }
}
