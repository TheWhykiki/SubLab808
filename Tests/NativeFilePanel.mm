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

// NSApplication's public event retrieval method treats nil as an immediate
// poll. Preserve that behavior, and only shorten later default-mode deadlines.
static bool boundedApplicationEventPollingEnabled = false;
static NSUInteger applicationStartEventDefaultModeDequeueCount = 0;
static NSUInteger applicationSettleEventDefaultModeDequeueCount = 0;
static NSUInteger applicationStopEventDefaultModeDequeueCount = 0;

static constexpr short applicationControlEventSubtype = 0x574b;
static constexpr NSInteger applicationControlEventMagic = 0x5748594b;
static constexpr NSInteger applicationStartEventCode = 1;
static constexpr NSInteger applicationSettleEventCode = 2;
static constexpr NSInteger applicationStopEventCode = 3;

@interface WhyKikiPresetTestApplication : NSApplication
@end

@implementation WhyKikiPresetTestApplication
- (NSEvent*)nextEventMatchingMask:(NSEventMask)mask
                        untilDate:(NSDate*)expiration
                           inMode:(NSRunLoopMode)mode
                          dequeue:(BOOL)dequeue
{
    NSDate* boundedExpiration = expiration;
    if (boundedApplicationEventPollingEnabled
        && expiration != nil
        && [mode isEqualToString:NSDefaultRunLoopMode])
    {
        auto* maximumExpiration = [NSDate dateWithTimeIntervalSinceNow:0.01];
        if ([expiration compare:maximumExpiration] == NSOrderedDescending)
            boundedExpiration = maximumExpiration;
    }
    auto* event = [super nextEventMatchingMask:mask
                                     untilDate:boundedExpiration
                                        inMode:mode
                                       dequeue:dequeue];
    if (boundedApplicationEventPollingEnabled
        && dequeue
        && [mode isEqualToString:NSDefaultRunLoopMode]
        && event != nil
        && [event type] == NSEventTypeApplicationDefined
        && [event subtype] == applicationControlEventSubtype
        && [event data1] == applicationControlEventMagic)
    {
        switch ([event data2])
        {
            case applicationStartEventCode: ++applicationStartEventDefaultModeDequeueCount; break;
            case applicationSettleEventCode: ++applicationSettleEventDefaultModeDequeueCount; break;
            case applicationStopEventCode: ++applicationStopEventDefaultModeDequeueCount; break;
            default: break;
        }
    }
    return event;
}
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
bool applicationStartEventWasCurrentEvent = false;
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

bool isRunnableApplicationEventContext() noexcept
{
    return [NSThread isMainThread] && NSApp != nil && [NSApp isRunning]
        && [NSApp modalWindow] == nil;
}

bool isDefaultApplicationRunLoopMode() noexcept
{
    auto* mode = [[NSRunLoop currentRunLoop] currentMode];
    return mode != nil && [mode isEqualToString:NSDefaultRunLoopMode];
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
        // Queue insertion is asynchronous. The test NSApplication's bounded
        // outer retrieval wait makes this event visible without a nested pump.
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
void NativeFilePanel::installTestApplication()
{
    @autoreleasepool
    {
        if (![NSThread isMainThread])
            throw std::runtime_error("NATIVE_PANEL_SETUP: test application requires the main thread");
        if (NSApp == nil)
            [WhyKikiPresetTestApplication sharedApplication];
        if ([NSApp class] != [WhyKikiPresetTestApplication class])
            throw std::runtime_error("NATIVE_PANEL_SETUP: test application was not installed before JUCE");
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
        if ([NSApp class] != [WhyKikiPresetTestApplication class])
            throw std::runtime_error("NATIVE_PANEL_SETUP: bounded test application is not installed");
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
        applicationStartEventWasCurrentEvent = false;
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
        applicationStartEventDefaultModeDequeueCount = 0;
        applicationSettleEventDefaultModeDequeueCount = 0;
        applicationStopEventDefaultModeDequeueCount = 0;
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
                                               applicationStartEventWasCurrentEvent =
                                                   [NSApp currentEvent] == event;
                                               std::fprintf(stderr,
                                                            "NATIVE_APP_LOOP_START_HANDLED count=%lu running=%d currentEvent=%d defaultDequeues=%lu\n",
                                                            static_cast<unsigned long>(applicationStartEventCount),
                                                            running ? 1 : 0,
                                                            applicationStartEventWasCurrentEvent ? 1 : 0,
                                                            static_cast<unsigned long>(applicationStartEventDefaultModeDequeueCount));
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
                                                            "NATIVE_APP_LOOP_SETTLE_HANDLED count=%lu running=%d postedFromReady=%d currentEvent=%d modalWindow=%d defaultDequeues=%lu\n",
                                                            static_cast<unsigned long>(applicationSettleEventCount),
                                                            running ? 1 : 0,
                                                            applicationSettleEventPostedFromReadyContext ? 1 : 0,
                                                            applicationSettleEventWasCurrentEvent ? 1 : 0,
                                                            applicationSettleEventHandledWithoutModalWindow ? 0 : 1,
                                                            static_cast<unsigned long>(applicationSettleEventDefaultModeDequeueCount));
                                               std::fflush(stderr);
                                               if (! NativeFilePanel::applicationSettleEventWasHandled())
                                               {
                                                   std::fputs("NATIVE_APP_LOOP_SETTLE_INVALID\n", stderr);
                                                   std::fflush(stderr);
                                                   std::terminate();
                                               }
                                               // STOP is only queued here. This SETTLE sendEvent:
                                               // invocation must return before the outer loop can
                                               // retrieve and dispatch STOP separately.
                                               if (! NativeFilePanel::postApplicationStopEvent())
                                               {
                                                   std::fputs("NATIVE_APP_LOOP_STOP_POST_FAILED\n", stderr);
                                                   std::fflush(stderr);
                                                   std::terminate();
                                               }
                                               std::fputs("NATIVE_APP_LOOP_STOP_POSTED\n", stderr);
                                               std::fflush(stderr);
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
                                               applicationStopCallbackSucceeded = applicationStopEventCount == 1
                                                   && NativeFilePanel::applicationSettleEventWasHandled()
                                                   && running
                                                   && applicationStopEventPostedFromReadyContext
                                                   && applicationStopEventWasCurrentEvent
                                                   && applicationStopEventHandledWithoutModalWindow
                                                   && applicationStopEventDefaultModeDequeueCount == 1
                                                   && applicationStopCallback != nullptr
                                                   && applicationStopCallback();
                                               std::fprintf(stderr,
                                                            "NATIVE_APP_LOOP_STOP_HANDLED count=%lu running=%d postedFromReady=%d currentEvent=%d modalWindow=%d defaultDequeues=%lu callback=%d\n",
                                                            static_cast<unsigned long>(applicationStopEventCount),
                                                            running ? 1 : 0,
                                                            applicationStopEventPostedFromReadyContext ? 1 : 0,
                                                            applicationStopEventWasCurrentEvent ? 1 : 0,
                                                            applicationStopEventHandledWithoutModalWindow ? 0 : 1,
                                                            static_cast<unsigned long>(applicationStopEventDefaultModeDequeueCount),
                                                            applicationStopCallbackSucceeded ? 1 : 0);
                                               std::fflush(stderr);
                                               return nil;
                                           }
                                           return event;
                                       }];
        if (applicationEventMonitor == nil)
        {
            applicationStopCallback = nullptr;
            throw std::runtime_error("NATIVE_PANEL_SETUP: cannot install application event monitor");
        }
        boundedApplicationEventPollingEnabled = true;
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
            && applicationStartEventHandledWhileRunning
            && applicationStartEventWasCurrentEvent
            && applicationStartEventDefaultModeDequeueCount == 1;
    }
}
bool NativeFilePanel::applicationIsReadyForSettleEvent() noexcept
{
    @autoreleasepool
    {
        return applicationStartEventWasHandled()
            && boundedApplicationEventPollingEnabled
            && ! applicationSettleEventPosted
            && applicationSettleEventCount == 0
            && ! applicationStopEventPosted
            && applicationStopEventCount == 0
            && isRunnableApplicationEventContext()
            && isDefaultApplicationRunLoopMode();
    }
}
bool NativeFilePanel::postApplicationSettleEvent() noexcept
{
    @autoreleasepool
    {
        const auto postedFromReadyContext = applicationIsReadyForSettleEvent();
        if (! postedFromReadyContext)
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
            && applicationSettleEventHandledWithoutModalWindow
            && applicationSettleEventDefaultModeDequeueCount == 1;
    }
}
bool NativeFilePanel::applicationIsReadyForStopEvent() noexcept
{
    @autoreleasepool
    {
        return applicationSettleEventWasHandled()
            && ! applicationStopEventPosted
            && applicationStopEventCount == 0
            && isRunnableApplicationEventContext();
    }
}
bool NativeFilePanel::postApplicationStopEvent() noexcept
{
    @autoreleasepool
    {
        const auto postedFromReadyContext = applicationIsReadyForStopEvent();
        if (! postedFromReadyContext || applicationStopCallback == nullptr)
            return false;
        applicationStopEventPosted = postApplicationControlEvent(applicationStopEventCode);
        applicationStopEventPostedFromReadyContext =
            postedFromReadyContext && applicationStopEventPosted;
        return applicationStopEventPosted;
    }
}
bool NativeFilePanel::applicationStopEventWasPosted() noexcept
{
    @autoreleasepool
    {
        return [NSThread isMainThread] && applicationStopEventPosted
            && applicationStopEventPostedFromReadyContext;
    }
}
bool NativeFilePanel::applicationStopEventWasHandled() noexcept
{
    @autoreleasepool
    {
        return applicationStopEventWasPosted()
            && applicationStopEventCount == 1
            && applicationStopEventHandledWhileRunning
            && applicationStopEventWasCurrentEvent
            && applicationStopEventHandledWithoutModalWindow
            && applicationStopEventDefaultModeDequeueCount == 1
            && applicationStopCallbackSucceeded;
    }
}
void NativeFilePanel::finishTestApplication() noexcept
{
    @autoreleasepool
    {
        if (![NSThread isMainThread])
            return;
        boundedApplicationEventPollingEnabled = false;
        if (applicationEventMonitor == nil)
            return;
        // JUCE's macOS stop path uses periodic events to release
        // NSApplication::run. Retire that wake before framework teardown.
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
