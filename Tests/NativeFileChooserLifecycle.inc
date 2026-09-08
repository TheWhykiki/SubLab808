// Included inside PresetTests.cpp's private test namespace so this focused suite
// reuses the real editor/state/library assertions without exposing product hooks.
// Cocoa observation lives in a separate macOS-only .mm file.
#if JUCE_MAC
struct NativeFixtureSnapshot
{
    std::set<juce::String> directories;
    std::map<juce::String, juce::MemoryBlock> files;
    explicit NativeFixtureSnapshot(const juce::File& root)
    {
        for (const auto& directory : root.findChildFiles(juce::File::findDirectories, true))
            directories.insert(directory.getRelativePathFrom(root));
        for (const auto& file : root.findChildFiles(juce::File::findFiles, true))
        {
            juce::MemoryBlock bytes;
            require(file.loadFileAsData(bytes), "read complete native-chooser fixture");
            files.emplace(file.getRelativePathFrom(root), std::move(bytes));
        }
    }
    void unchanged(const juce::File& root) const
    {
        const NativeFixtureSnapshot after(root);
        require(after.directories == directories && after.files == files,
                "native chooser cancellation preserves the complete fixture file tree and bytes");
    }
};

struct NativeChooserCase
{
    juce::File root;
    bool importing = false;
    OwnerAction action = OwnerAction::hideAncestor;
};

struct NativeChooserCaseState
{
    explicit NativeChooserCaseState(NativeChooserCase caseToRun)
        : testCase(std::move(caseToRun)) {}

    NativeChooserCase testCase;
    juce::File external, input, output;
    std::unique_ptr<Processor> processor;
    std::unique_ptr<LifecycleEditor> owner;
    std::unique_ptr<LifecycleSnapshot> stateBefore;
    std::unique_ptr<NativeFixtureSnapshot> filesBefore;
    std::unique_ptr<LifecycleSnapshot> reopenedBeforeInteraction;
    std::unique_ptr<LifecycleSnapshot> postInteractionState;
    juce::Component::SafePointer<juce::Component> managementMenu;
    juce::Component::SafePointer<juce::Component> nativeModal;
    juce::Component::SafePointer<juce::AlertWindow> reopenedDialog;
    std::unique_ptr<NativeFilePanel> panel;
    juce::Button* reopenedToggle = nullptr;
    juce::RangedAudioParameter* reopenedParameter = nullptr;
    float reopenedInitialValue = 0.0f;
    std::string nativeClass;
};

bool nativeApplicationStartEventPosted = false;
bool nativeApplicationSettleEventPosted = false;
bool nativeApplicationWasRunningAtStop = false;
std::atomic<bool> nativeApplicationLoopReturned { false };
std::atomic<bool> nativeApplicationReturnWatchdogArmed { false };

enum class NativeApplicationShutdownStage
{
    idle,
    watchdogArmed,
    settlePosted,
    stopAccepted,
    loopReturned
};

std::atomic<NativeApplicationShutdownStage> nativeApplicationShutdownStage {
    NativeApplicationShutdownStage::idle
};

const char* shutdownStageName(NativeApplicationShutdownStage stage) noexcept
{
    switch (stage)
    {
        case NativeApplicationShutdownStage::idle: return "idle";
        case NativeApplicationShutdownStage::watchdogArmed: return "watchdog-armed";
        case NativeApplicationShutdownStage::settlePosted: return "settle-posted";
        case NativeApplicationShutdownStage::stopAccepted: return "stop-accepted";
        case NativeApplicationShutdownStage::loopReturned: return "loop-returned";
    }
    return "unknown";
}

void nativeApplicationReturnWatchdog(void*) noexcept
{
    if (nativeApplicationLoopReturned.load(std::memory_order_acquire)) return;
    std::fprintf(stderr,
                 "NATIVE_APP_LOOP_RETURN_TIMEOUT: NSApplication::run did not return within five seconds after the shutdown handshake was armed (stage=%s)\n",
                 shutdownStageName(nativeApplicationShutdownStage.load(std::memory_order_acquire)));
    std::fflush(stderr);
    std::terminate();
}

bool armNativeApplicationReturnWatchdog() noexcept
{
    bool expected = false;
    if (! nativeApplicationReturnWatchdogArmed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return false;
    nativeApplicationShutdownStage.store(NativeApplicationShutdownStage::watchdogArmed,
                                         std::memory_order_release);
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
                     dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0),
                     nullptr, nativeApplicationReturnWatchdog);
    return true;
}

bool stopJuceApplicationLoopFromEvent() noexcept
{
    nativeApplicationWasRunningAtStop = NativeFilePanel::applicationIsRunning();
    std::fprintf(stderr, "NATIVE_APP_LOOP_STOP_BEGIN running=%d\n",
                 nativeApplicationWasRunningAtStop ? 1 : 0);
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    if (messageManager == nullptr)
    {
        std::fputs("NATIVE_APP_LOOP_STOP_FAILED: message manager missing\n", stderr);
        std::fflush(stderr);
        return false;
    }
    if (messageManager->hasStopMessageBeenSent())
    {
        std::fputs("NATIVE_APP_LOOP_STOP_FAILED: stop was already requested\n", stderr);
        std::fflush(stderr);
        return false;
    }
    messageManager->stopDispatchLoop();
    const auto accepted = messageManager->hasStopMessageBeenSent();
    if (accepted)
        nativeApplicationShutdownStage.store(NativeApplicationShutdownStage::stopAccepted,
                                             std::memory_order_release);
    return accepted;
}

class NativeChooserCoordinator final : private juce::Timer
{
public:
    NativeChooserCoordinator(juce::File suiteRoot, const juce::String& requestedCase)
    {
        for (const bool importing : { true, false })
            for (const auto action : { OwnerAction::hideAncestor, OwnerAction::detach,
                                       OwnerAction::destroy, OwnerAction::hideThenDestroy })
            {
                const auto name = juce::String(importing ? "import-" : "export-") + actionName(action);
                if (requestedCase.isNotEmpty() && requestedCase != name) continue;
                cases.push_back({ suiteRoot.getChildFile(juce::String(importing ? "Import-" : "Export-")
                                                         + actionName(action)), importing, action });
            }

        if (requestedCase.isEmpty())
        {
            // Close the cycle: the final export/hide-then-destroy transition must
            // not poison the first kind of native session in the same app loop.
            cases.push_back({ suiteRoot.getChildFile("Wraparound-Import-ancestor-hide"),
                              true, OwnerAction::hideAncestor });
        }

        require(! cases.empty(), "WHYKIKI_PRESET_TEST_NATIVE_CASE names a supported native chooser case");
    }

    ~NativeChooserCoordinator() override
    {
        // The normal shutdown path stops this timer before posting SETTLE.
        // Keep an idempotent most-derived guard before
        // retained case members are destroyed and the Timer base is reached.
        stopTimer();
    }

    void start()
    {
        require(juce::MessageManager::getInstance()->isThisTheMessageThread(),
                "native chooser coordinator starts on the message thread");
        // NSApplication::run may service Timer sources during launch. The first
        // phase remains gated until AppKit dispatches our private START NSEvent.
        startupDeadline = now() + 5000.0;
        startTimer(10);
    }

    bool completedNormally() const { return completed; }
    const std::string& failureMessage() const { return failure; }
    std::size_t caseCount() const { return cases.size(); }

    void verifyFinalState()
    {
        require(failure.empty(), "native chooser final audit requires a successful lifecycle");
        require(shutdownStarted && current == nullptr && caseIndex == cases.size(),
                "native chooser reached run-loop return from the completed shutdown state");
        require(NativeFilePanel::applicationStopEventWasHandled(),
                "native chooser stop event ran exactly once in a valid clean context");
        auditRetiredCases();
        auditGlobalState();
    }

private:
    enum class Phase
    {
        createCase,
        awaitHostActive,
        awaitManagementGone,
        awaitNativeModal,
        awaitPanel,
        fixtureFence,
        awaitNativeModalGone,
        awaitPanelRetired,
        retirementFence,
        awaitReopenedHost,
        awaitReopenedDialog,
        awaitReopenedDialogGone,
        awaitToggleOn,
        awaitToggleOff,
        destroyCase,
        betweenCases,
        failureDestroy,
        failureAwaitRetired,
        failureDrain,
        awaitApplicationStopReady,
        awaitApplicationReturn
    };

    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    void enter(Phase next, double timeoutMs = 0.0)
    {
        phase = next;
        deadline = timeoutMs > 0.0 ? now() + timeoutMs : 0.0;
        fenceTurns = 0;
    }

    bool waitUntil(bool ready, const char* timeoutMessage) const
    {
        if (ready) return true;
        if (deadline > 0.0 && now() >= deadline) throw std::runtime_error(timeoutMessage);
        return false;
    }

    static void disableAutomaticAnimations(juce::Component& component)
    {
        auto* peer = component.getPeer();
        require(peer != nullptr, "test-owned component resolves to a containing native desktop peer");
        NativeFilePanel::disableAutomaticWindowAnimations(peer->getNativeHandle());
    }

    void auditRetiredCases()
    {
        for (const auto& retired : retiredCases)
        {
            auto& state = *retired;
            require(state.owner == nullptr && state.nativeModal == nullptr
                        && state.reopenedDialog == nullptr && state.managementMenu == nullptr,
                    "retired native chooser case has no live editor or modal owner");
            require(state.panel != nullptr && ! state.panel->isAlive()
                        && ! state.panel->isVisible() && ! state.panel->hasDelegate()
                        && state.panel->completionProgressIsValid()
                        && state.panel->completionIsQuiescent()
                        && state.panel->markSafeOwnerRetired(),
                    "retired native chooser case remains quiescent and owner-safe");
            require(state.postInteractionState != nullptr,
                    "retired native chooser case has a final state snapshot");
            state.postInteractionState->unchanged(*state.processor);
            state.filesBefore->unchanged(state.testCase.root);
            require(! state.output.exists(),
                    "retired cancelled export never creates its target");
        }
    }

    void timerCallback() override
    {
        try
        {
            if (! applicationLoopStarted)
            {
                if (! NativeFilePanel::applicationStartEventWasHandled())
                {
                    if (now() >= startupDeadline)
                    {
                        std::fprintf(stderr,
                                     "NATIVE_APP_LOOP_START_TIMEOUT running=%d\n",
                                     NativeFilePanel::applicationIsRunning() ? 1 : 0);
                        std::fflush(stderr);
                        std::terminate();
                    }
                    return;
                }
                applicationLoopStarted = true;
                // Reserve room below CTest's 60/480-second process watchdogs
                // for bounded failure cleanup plus the three shutdown stages.
                runDeadline = now() + (cases.size() == 1 ? 32000.0 : 435000.0);
                enter(Phase::createCase);
                std::fputs("NATIVE_APP_LOOP_ACTIVE dispatch=1 running=1\n", stderr);
            }
            if (failure.empty())
            {
                auditRetiredCases();
                if (shutdownStarted) auditGlobalState();
            }
            if (! shutdownStarted && failure.empty()
                && runDeadline > 0.0 && now() >= runDeadline)
                throw std::runtime_error(
                    std::string("NATIVE_CHOOSER_RUN_TIMEOUT: overall deadline reached in case ")
                    + std::to_string(caseIndex + 1) + "/" + std::to_string(cases.size())
                    + " during " + phaseName(phase));
            advance();
        }
        catch (const std::exception& exception)
        {
            shutdownStarted ? failDuringShutdown(exception.what()) : fail(exception.what());
        }
        catch (...)
        {
            shutdownStarted ? failDuringShutdown("unknown exception in native chooser shutdown")
                            : fail("unknown exception in native chooser coordinator");
        }
    }

    void advance()
    {
        require(juce::MessageManager::getInstance()->isThisTheMessageThread(),
                "native chooser phases run on the message thread");

        switch (phase)
        {
            case Phase::createCase: createCase(); return;
            case Phase::awaitHostActive: awaitHostActive(); return;
            case Phase::awaitManagementGone: awaitManagementGone(); return;
            case Phase::awaitNativeModal: awaitNativeModal(); return;
            case Phase::awaitPanel: awaitPanel(); return;
            case Phase::fixtureFence: fixtureFence(); return;
            case Phase::awaitNativeModalGone: awaitNativeModalGone(); return;
            case Phase::awaitPanelRetired: awaitPanelRetired(); return;
            case Phase::retirementFence: retirementFence(); return;
            case Phase::awaitReopenedHost: awaitReopenedHost(); return;
            case Phase::awaitReopenedDialog: awaitReopenedDialog(); return;
            case Phase::awaitReopenedDialogGone: awaitReopenedDialogGone(); return;
            case Phase::awaitToggleOn: awaitToggleOn(); return;
            case Phase::awaitToggleOff: awaitToggleOff(); return;
            case Phase::destroyCase: destroyCase(); return;
            case Phase::betweenCases: betweenCases(); return;
            case Phase::failureDestroy: failureDestroy(); return;
            case Phase::failureAwaitRetired: failureAwaitRetired(); return;
            case Phase::failureDrain: failureDrain(); return;
            case Phase::awaitApplicationStopReady: awaitApplicationStopReady(); return;
            case Phase::awaitApplicationReturn: awaitApplicationReturn(); return;
        }
        throw std::runtime_error("unknown native chooser phase");
    }

    static const char* phaseName(Phase value)
    {
        switch (value)
        {
            case Phase::createCase: return "createCase";
            case Phase::awaitHostActive: return "awaitHostActive";
            case Phase::awaitManagementGone: return "awaitManagementGone";
            case Phase::awaitNativeModal: return "awaitNativeModal";
            case Phase::awaitPanel: return "awaitPanel";
            case Phase::fixtureFence: return "fixtureFence";
            case Phase::awaitNativeModalGone: return "awaitNativeModalGone";
            case Phase::awaitPanelRetired: return "awaitPanelRetired";
            case Phase::retirementFence: return "retirementFence";
            case Phase::awaitReopenedHost: return "awaitReopenedHost";
            case Phase::awaitReopenedDialog: return "awaitReopenedDialog";
            case Phase::awaitReopenedDialogGone: return "awaitReopenedDialogGone";
            case Phase::awaitToggleOn: return "awaitToggleOn";
            case Phase::awaitToggleOff: return "awaitToggleOff";
            case Phase::destroyCase: return "destroyCase";
            case Phase::betweenCases: return "betweenCases";
            case Phase::failureDestroy: return "failureDestroy";
            case Phase::failureAwaitRetired: return "failureAwaitRetired";
            case Phase::failureDrain: return "failureDrain";
            case Phase::awaitApplicationStopReady: return "awaitApplicationStopReady";
            case Phase::awaitApplicationReturn: return "awaitApplicationReturn";
        }
        return "unknown";
    }

    void createCase()
    {
        require(current == nullptr && caseIndex < cases.size(), "native chooser case index is valid");
        require(juce::Component::getNumCurrentlyModalComponents() == 0,
                "native chooser case starts without a JUCE modal");
        require(NativeFilePanel::visibleCount() == 0,
                "native chooser case starts without a native file panel");
        require(! NativeFilePanel::hasActiveCompletionSession(),
                "native chooser case starts without an unretired completion observer");

        current = std::make_unique<NativeChooserCaseState>(cases[caseIndex]);
        const auto& testCase = current->testCase;
        std::printf("NATIVE_CHOOSER_BEGIN: %s / %s\n",
                    testCase.importing ? "Import" : "Export", actionName(testCase.action));
        ok(testCase.root.createDirectory());
        current->external = testCase.root.getChildFile("External");
        ok(current->external.createDirectory());
        current->processor = std::make_unique<Processor>(testCase.root.getChildFile("Library"));
        current->input = current->external.getChildFile("Input" + current->processor->presets.extension());
        current->output = current->external.getChildFile("MustNotBeWritten" + current->processor->presets.extension());
        ok(current->processor->presets.exportCurrent(current->input));
        require(current->input.existsAsFile() && ! current->output.exists(),
                "fixture input exists and export destination does not");
        current->owner = std::make_unique<LifecycleEditor>(*current->processor, DeferredInitialPumpTag {});
        juce::Process::makeForegroundProcess();
        current->owner->window->toFront(true);
        enter(Phase::awaitHostActive, 10000.0);
    }

    void awaitHostActive()
    {
        auto& state = *current;
        juce::Process::makeForegroundProcess();
        state.owner->window->toFront(true);
        if (! waitUntil(juce::Process::isForegroundProcess() && state.owner->window->isActiveWindow(),
                        "NATIVE_PANEL_SETUP_TIMEOUT: the test process/window did not become active")) return;

        state.stateBefore = std::make_unique<LifecycleSnapshot>(*state.processor);
        state.filesBefore = std::make_unique<NativeFixtureSnapshot>(state.testCase.root);
        auto* menu = openManagement(*state.owner->editor);
        state.managementMenu = menu;
        disableAutomaticAnimations(*menu);
        menu->exitModalState(state.testCase.importing ? 3 : 4);
        enter(Phase::awaitManagementGone, 2000.0);
    }

    void awaitManagementGone()
    {
        if (! waitUntil(current->managementMenu == nullptr,
                        "Modal component was not destroyed: management menu selection")) return;
        std::puts("UI teardown: management menu selection destroyed");
        enter(Phase::awaitNativeModal, 10000.0);
    }

    void awaitNativeModal()
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        if (! waitUntil(modal != nullptr,
                        "NATIVE_PANEL_SETUP_TIMEOUT: JUCE native file-chooser modal did not appear")) return;
        require(dynamic_cast<juce::AlertWindow*>(modal) == nullptr
                    && dynamic_cast<juce::CallOutBox*>(modal) == nullptr,
                "the product created a real file-chooser modal");
        current->nativeModal = modal;
        disableAutomaticAnimations(*modal);
        enter(Phase::awaitPanel, 10000.0);
    }

    void awaitPanel()
    {
        auto panel = NativeFilePanel::findVisible(current->testCase.importing,
            current->testCase.importing ? "Import preset" : "Export current sound");
        if (! waitUntil(panel != nullptr,
                        "NATIVE_PANEL_SETUP_TIMEOUT: real NSSavePanel/NSOpenPanel did not become visible")) return;
        current->panel = std::move(panel);
        require(current->panel->hasDelegate(), "native file panel has its real JUCE delegate before teardown");
        require(current->panel->moduleWasRetainedAtBegin(),
                "the product retained its module before AppKit copied the native completion");
        require(current->panel->beganExactlyOnce() && current->panel->completionHasNotStarted(),
                "visible native file panel has one active, not-yet-completed AppKit session");
        require(NativeFilePanel::visibleCount() == 1,
                "exactly one native file panel belongs to this test process");
        current->nativeClass = current->panel->className();
        current->panel->useFixtureLocation(current->external.getFullPathName().toStdString(),
                                           (current->testCase.importing ? current->input : current->output)
                                               .getFileName().toStdString());
        enter(Phase::fixtureFence);
    }

    void fixtureFence()
    {
        // Two timer turns return control to the one real NSApplication loop so
        // AppKit can finish directory/name synchronisation without a nested pump.
        if (++fenceTurns < 2) return;
        auto& state = *current;
        require(state.nativeModal != nullptr && state.nativeModal->isCurrentlyModal()
                    && state.panel->isVisible(),
                "native panel and JUCE modal are both active before owner transition");
        require(state.panel->completionHasNotStarted(),
                "native panel completion has not started before the owner transition");
        state.stateBefore->unchanged(*state.processor);
        state.filesBefore->unchanged(state.testCase.root);

        std::printf("NATIVE_CHOOSER_OWNER_ACTION_BEGIN: %s / %s\n",
                    state.testCase.importing ? "Import" : "Export", actionName(state.testCase.action));
        state.owner->apply(state.testCase.action);
        std::printf("NATIVE_CHOOSER_OWNER_ACTION_END: %s / %s\n",
                    state.testCase.importing ? "Import" : "Export", actionName(state.testCase.action));
        if (state.testCase.action == OwnerAction::destroy
            || state.testCase.action == OwnerAction::hideThenDestroy)
        {
            require(state.nativeModal == nullptr && ! state.panel->isVisible() && ! state.panel->hasDelegate(),
                    "editor destruction synchronously clears the JUCE modal, native visibility and JUCE delegate");
        }
        else
        {
            require(state.nativeModal != nullptr && state.nativeModal->isCurrentlyModal()
                        && state.panel->isVisible() && state.panel->hasDelegate(),
                    "hide/detach returns before non-reentrant native chooser teardown runs on a later timer event");
        }
        enter(Phase::awaitNativeModalGone, 2000.0);
    }

    void awaitNativeModalGone()
    {
        if (! waitUntil(current->nativeModal == nullptr,
                        "Modal component was not destroyed: native file-chooser modal")) return;
        std::puts("UI teardown: native file-chooser modal destroyed");
        enter(Phase::awaitPanelRetired, 10000.0);
    }

    void awaitPanelRetired()
    {
        require(current->panel->completionProgressIsValid(),
                "native file panel completion-session counts became invalid");
        const bool ownerRetired = current->nativeModal == nullptr
                               && ! current->panel->isVisible()
                               && ! current->panel->hasDelegate()
                               && ! current->panel->isAlive();
        if (! waitUntil(ownerRetired,
                        "NATIVE_PANEL_TEARDOWN_TIMEOUT: JUCE modal or native panel/delegate/window remains active")) return;
        require(current->panel->completionIsQuiescent(),
                "native completion was executing at the safe owner-retirement boundary");
        require(current->panel->markSafeOwnerRetired(),
                "native completion observer could not mark safe owner retirement");
        require(! NativeFilePanel::hasActiveCompletionSession(),
                "native completion remained active after owner retirement");
        enter(Phase::retirementFence);
    }

    void retirementFence()
    {
        // JUCE's SafePointer-bearing native owner is gone. Keep returning to the
        // real app loop; any later AppKit entry must return before the next turn.
        if (++fenceTurns < 3) return;
        auto& state = *current;
        require(state.nativeModal == nullptr && ! state.panel->isAlive()
                    && state.panel->completionIsQuiescent()
                    && state.panel->markSafeOwnerRetired()
                    && NativeFilePanel::visibleCount() == 0
                    && ! NativeFilePanel::hasActiveCompletionSession()
                    && juce::Component::getNumCurrentlyModalComponents() == 0,
                "no native or JUCE modal remains after owner transition");
        state.stateBefore->unchanged(*state.processor);
        state.filesBefore->unchanged(state.testCase.root);
        require(! state.output.exists(), "cancelled export never creates its target");
        state.owner->beginReopen(state.testCase.action);
        enter(Phase::awaitReopenedHost, 10000.0);
    }

    void awaitReopenedHost()
    {
        auto& state = *current;
        juce::Process::makeForegroundProcess();
        state.owner->window->toFront(true);
        if (! waitUntil(state.owner->editor != nullptr && state.owner->editor->isShowing()
                            && state.owner->window->isActiveWindow(),
                        "NATIVE_PANEL_REOPEN_TIMEOUT: replacement editor did not become active")) return;
        require(juce::Component::getNumCurrentlyModalComponents() == 0,
                "no orphan modal remains before reopened-control probe");
        state.stateBefore->unchanged(*state.processor);
        require(! state.owner->editor->isCurrentlyBlockedByAnotherModalComponent(),
                "reopened editor is not modal-blocked");
        auto& saveAs = lifecycleButton(*state.owner->editor, "Save preset as");
        require(! saveAs.isCurrentlyBlockedByAnotherModalComponent(),
                "reopened Save As is not modal-blocked");
        auto* accessibility = saveAs.getAccessibilityHandler();
        require(accessibility != nullptr && accessibility->getCurrentState().isFocusable(),
                "reopened Save As is accessibility-focusable");
        state.reopenedBeforeInteraction = std::make_unique<LifecycleSnapshot>(*state.processor);
        saveAs.triggerClick();
        enter(Phase::awaitReopenedDialog, 10000.0);
    }

    void awaitReopenedDialog()
    {
        auto* modal = juce::Component::getCurrentlyModalComponent();
        if (! waitUntil(modal != nullptr,
                        "NATIVE_PANEL_REOPEN_TIMEOUT: Save As probe did not open")) return;
        auto* alert = dynamic_cast<juce::AlertWindow*>(modal);
        require(alert != nullptr && alert->getName() == "Save preset as",
                "real reopened Save As dialog opened");
        current->reopenedDialog = alert;
        disableAutomaticAnimations(*alert);
        alert->exitModalState(0);
        enter(Phase::awaitReopenedDialogGone, 2000.0);
    }

    void awaitReopenedDialogGone()
    {
        if (! waitUntil(current->reopenedDialog == nullptr,
                        "Modal component was not destroyed: reopened Save As Cancel")) return;
        std::puts("UI teardown: reopened Save As Cancel destroyed");
        auto& state = *current;
        state.reopenedBeforeInteraction->unchanged(*state.processor);
#if PRESET_TEST_SUBLAB
        state.reopenedToggle = &lifecycleButton(*state.owner->editor, "One Shot playback mode");
        state.reopenedParameter = state.processor->parameters.getParameter("oneshot");
#else
        state.reopenedToggle = &lifecycleButton(*state.owner->editor, "FREEZE");
        state.reopenedParameter = state.processor->parameters.getParameter("freeze");
#endif
        require(state.reopenedParameter != nullptr, "reopened toggle parameter exists");
        require(! state.reopenedToggle->isCurrentlyBlockedByAnotherModalComponent(),
                "reopened sound control is not modal-blocked");
        auto* accessibility = state.reopenedToggle->getAccessibilityHandler();
        require(accessibility != nullptr && accessibility->getCurrentState().isFocusable(),
                "reopened sound control is accessibility-focusable");
        state.reopenedInitialValue = state.reopenedParameter->getValue();
        state.reopenedToggle->triggerClick();
        enter(Phase::awaitToggleOn, 2000.0);
    }

    void awaitToggleOn()
    {
        auto& state = *current;
        if (! waitUntil(std::abs(state.reopenedParameter->getValue() - state.reopenedInitialValue) > 0.5f,
                        "reopened sound-control click did not update its actual parameter")) return;
        state.reopenedToggle->triggerClick();
        enter(Phase::awaitToggleOff, 2000.0);
    }

    void awaitToggleOff()
    {
        auto& state = *current;
        if (! waitUntil(std::abs(state.reopenedParameter->getValue() - state.reopenedInitialValue) <= 0.0f,
                        "second sound-control click did not restore its actual parameter")) return;
        require(juce::Component::getNumCurrentlyModalComponents() == 0,
                "no modal remains after reopened-control probe");
        state.reopenedBeforeInteraction->libraryUnchanged(*state.processor);
        state.filesBefore->unchanged(state.testCase.root);
        require(! state.output.exists(), "cancelled export never creates its target");
        state.postInteractionState = std::make_unique<LifecycleSnapshot>(*state.processor);
        enter(Phase::destroyCase);
    }

    void destroyCase()
    {
        require(current != nullptr && current->postInteractionState != nullptr,
                "native chooser case has a final audit snapshot before retirement");
        current->postInteractionState->unchanged(*current->processor);
        current->filesBefore->unchanged(current->testCase.root);
        require(! current->output.exists(), "cancelled export never creates its target");
        require(current->panel->completionProgressIsValid()
                    && current->panel->completionIsQuiescent()
                    && current->panel->markSafeOwnerRetired(),
                "native completion remains valid before retiring the reopened editor");
        lastImporting = current->testCase.importing;
        lastAction = current->testCase.action;
        lastNativeClass = current->nativeClass;
        lastLateCompletionEntries = current->panel->lateCompletionEntryCount();
        current->reopenedToggle = nullptr;
        current->reopenedParameter = nullptr;
        current->owner.reset();
        retiredCases.push_back(std::move(current));
        auditRetiredCases();
        enter(Phase::betweenCases, 10000.0);
    }

    void betweenCases()
    {
        const bool clean = globalStateIsClean();
        if (! waitUntil(clean,
                        "NATIVE_PANEL_TEARDOWN_TIMEOUT: case destruction left native or JUCE modal state")) return;
        if (++fenceTurns < 3) return;
        auditRetiredCases();
        lastLateCompletionEntries = retiredCases.back()->panel->lateCompletionEntryCount();
        std::printf("PASS: native %s / %s [%s]: real panel closed, delegate cleared, modal deleted, state/files unchanged, editor interactive (late AppKit completions=%zu; process total=%zu).\n",
                    lastImporting ? "Import" : "Export", actionName(lastAction), lastNativeClass.c_str(),
                    lastLateCompletionEntries,
                    NativeFilePanel::totalLateCompletionEntryCount());
        ++caseIndex;
        if (caseIndex < cases.size())
        {
            enter(Phase::createCase);
            return;
        }

        beginApplicationStopHandshake();
    }

    bool globalStateIsClean() const
    {
        return juce::Component::getNumCurrentlyModalComponents() == 0
            && NativeFilePanel::visibleCount() == 0
            && ! NativeFilePanel::hasActiveCompletionSession();
    }

    void auditGlobalState() const
    {
        require(globalStateIsClean(),
                "native chooser shutdown has no JUCE modal, native panel or completion session");
    }

    void beginApplicationStopHandshake() noexcept
    {
        if (shutdownStarted) return;
        shutdownStarted = true;
        completed = true;
        enter(Phase::awaitApplicationStopReady, 5000.0);
        std::fputs("NATIVE_APP_LOOP_STOP_READY_BEGIN\n", stderr);
        std::fflush(stderr);
        // Stable readiness is established on default-mode timer turns. The
        // final turn arms an independent watchdog and retires this recurring
        // source before queueing SETTLE for the outer application loop.
        startTimer(10);
    }

    [[noreturn]] void shutdownTimeout(const char* stage) const noexcept
    {
        std::fprintf(stderr,
                     "NATIVE_APP_LOOP_SHUTDOWN_TIMEOUT stage=%s readyForSettle=%d settle=%d readyForStop=%d stopPosted=%d stopHandled=%d running=%d\n",
                     stage,
                     NativeFilePanel::applicationIsReadyForSettleEvent() ? 1 : 0,
                     NativeFilePanel::applicationSettleEventWasHandled() ? 1 : 0,
                     NativeFilePanel::applicationIsReadyForStopEvent() ? 1 : 0,
                     NativeFilePanel::applicationStopEventWasPosted() ? 1 : 0,
                     NativeFilePanel::applicationStopEventWasHandled() ? 1 : 0,
                     NativeFilePanel::applicationIsRunning() ? 1 : 0);
        std::fflush(stderr);
        std::terminate();
    }

    void awaitApplicationStopReady()
    {
        if (! NativeFilePanel::applicationIsReadyForSettleEvent())
        {
            fenceTurns = 0;
            if (now() >= deadline) shutdownTimeout("ready");
            return;
        }
        if (++fenceTurns < 3) return;

        if (! armNativeApplicationReturnWatchdog())
        {
            std::fputs("NATIVE_APP_LOOP_RETURN_WATCHDOG_ARM_FAILED\n", stderr);
            std::fflush(stderr);
            std::terminate();
        }
        std::fputs("NATIVE_APP_LOOP_RETURN_WATCHDOG_ARMED\n", stderr);
        std::fflush(stderr);
        // stopTimer() is synchronous inside its callback. The test application
        // already bounds the outer nextEvent: wait, so this same-thread post
        // needs neither a worker nor a synthetic wake source.
        stopTimer();
        std::fputs("NATIVE_APP_LOOP_SETTLE_BEGIN\n", stderr);
        nativeApplicationSettleEventPosted = NativeFilePanel::postApplicationSettleEvent();
        if (! nativeApplicationSettleEventPosted)
        {
            std::fputs("NATIVE_APP_LOOP_SETTLE_POST_FAILED\n", stderr);
            std::fflush(stderr);
            std::terminate();
        }
        nativeApplicationShutdownStage.store(NativeApplicationShutdownStage::settlePosted,
                                             std::memory_order_release);
        std::fputs("NATIVE_APP_LOOP_SETTLE_POSTED\n", stderr);
        std::fflush(stderr);
        enter(Phase::awaitApplicationReturn);
    }

    [[noreturn]] void awaitApplicationReturn() const noexcept
    {
        shutdownTimeout("timer-fired-after-settle");
    }

    void failDuringShutdown(const char* message) noexcept
    {
        if (failure.empty())
        {
            failure = message;
            std::fprintf(stderr, "FAIL_DURING_SHUTDOWN: %s\n", failure.c_str());
        }
        else
        {
            std::fprintf(stderr, "FAILURE_CLEANUP_ERROR: %s\n", message);
        }
        std::fflush(stderr);
    }

    void fail(const char* message) noexcept
    {
        try
        {
            if (! failure.empty())
            {
                std::fprintf(stderr, "FAILURE_CLEANUP_ERROR: %s\n", message);
                std::fflush(stderr);
                beginApplicationStopHandshake();
                return;
            }
            failure = message;
            std::fprintf(stderr, "FAIL: %s\n", failure.c_str());
            std::fflush(stderr);
            stopTimer();
            if (current != nullptr)
            {
                if (current->reopenedDialog != nullptr) current->reopenedDialog->exitModalState(0);
                if (current->nativeModal != nullptr) current->nativeModal->exitModalState(0);
                if (current->managementMenu != nullptr) current->managementMenu->exitModalState(0);
            }
            // Destruction runs only after returning to the top-level application
            // loop, never re-entrantly in the callback that detected the failure.
            enter(current != nullptr ? Phase::failureDestroy : Phase::failureAwaitRetired,
                  current != nullptr ? 0.0 : 3000.0);
            startTimer(10);
        }
        catch (...)
        {
            std::fputs("FAILURE_CLEANUP_ERROR: initial cleanup threw; stopping app loop\n", stderr);
            std::fflush(stderr);
            beginApplicationStopHandshake();
        }
    }

    void failureDestroy()
    {
        if (++fenceTurns < 2) return;
        if (current == nullptr)
        {
            enter(Phase::failureAwaitRetired, 3000.0);
            return;
        }
        failureNativeModal = current->nativeModal;
        failurePanel = std::move(current->panel);
        current.reset();
        enter(Phase::failureAwaitRetired, 3000.0);
    }

    void failureAwaitRetired()
    {
        bool panelClean = failurePanel == nullptr && failureNativeModal == nullptr;
        if (failurePanel != nullptr
            && failureNativeModal == nullptr
            && ! failurePanel->isVisible()
            && ! failurePanel->hasDelegate()
            && ! failurePanel->isAlive()
            && failurePanel->completionIsQuiescent())
            panelClean = failurePanel->markSafeOwnerRetired();
        const bool globallyClean = globalStateIsClean();
        if (panelClean && globallyClean)
        {
            failurePanel.reset();
            enter(Phase::failureDrain, 100.0);
            return;
        }
        if (now() < deadline) return;

        std::fprintf(stderr,
                     "FAILURE_CLEANUP_TIMEOUT: modal=%d visiblePanels=%d activeCompletion=%d completionQuiescent=%d lateCompletions=%zu\n",
                     juce::Component::getNumCurrentlyModalComponents(), NativeFilePanel::visibleCount(),
                     NativeFilePanel::hasActiveCompletionSession(),
                     failurePanel == nullptr || failurePanel->completionIsQuiescent(),
                     NativeFilePanel::totalLateCompletionEntryCount());
        std::fflush(stderr);
        failurePanel.reset();
        enter(Phase::failureDrain, 100.0);
    }

    void failureDrain()
    {
        if (now() < deadline) return;
        beginApplicationStopHandshake();
    }

    std::vector<NativeChooserCase> cases;
    std::unique_ptr<NativeChooserCaseState> current;
    std::vector<std::unique_ptr<NativeChooserCaseState>> retiredCases;
    std::unique_ptr<NativeFilePanel> failurePanel;
    juce::Component::SafePointer<juce::Component> failureNativeModal;
    size_t caseIndex = 0;
    Phase phase = Phase::createCase;
    double deadline = 0.0;
    double startupDeadline = 0.0;
    double runDeadline = 0.0;
    int fenceTurns = 0;
    bool lastImporting = false;
    OwnerAction lastAction = OwnerAction::hideAncestor;
    std::string lastNativeClass;
    std::size_t lastLateCompletionEntries = 0;
    bool completed = false;
    bool applicationLoopStarted = false;
    bool shutdownStarted = false;
    std::string failure;
};

int runNativeFileChooserLifecycles(const juce::File& root, const juce::String& requestedCase = {})
{
    struct ApplicationCleanup final
    {
        ~ApplicationCleanup()
        {
            nativeFileChooserHarnessEnabled = false;
            NativeFilePanel::finishTestApplication();
        }
    } applicationCleanup;

    require(juce::MessageManager::getInstance()->isThisTheMessageThread(),
            "native chooser tests start on the message thread");
    require(! juce::Desktop::getInstance().getDisplays().displays.isEmpty(),
            "native chooser tests require an active macOS desktop session");
    require(juce::FileChooser::isPlatformDialogAvailable(),
            "native chooser platform implementation is available");
    NativeFilePanel::prepareTestApplication(stopJuceApplicationLoopFromEvent);
    nativeApplicationLoopReturned.store(false, std::memory_order_release);
    nativeApplicationReturnWatchdogArmed.store(false, std::memory_order_release);
    nativeApplicationShutdownStage.store(NativeApplicationShutdownStage::idle,
                                         std::memory_order_release);
    nativeApplicationStartEventPosted = false;
    nativeApplicationSettleEventPosted = false;
    nativeApplicationWasRunningAtStop = false;
    nativeFileChooserHarnessEnabled = true;
    NativeChooserCoordinator coordinator(root, requestedCase);
    nativeApplicationStartEventPosted = NativeFilePanel::postApplicationStartEvent();
    require(nativeApplicationStartEventPosted,
            "native chooser could not post its AppKit start event");
    std::fputs("NATIVE_APP_LOOP_START_POSTED\n", stderr);
    coordinator.start();
    std::fputs("NATIVE_APP_LOOP_ENTER\n", stderr);
    juce::MessageManager::getInstance()->runDispatchLoop();
    nativeApplicationLoopReturned.store(true, std::memory_order_release);
    nativeApplicationShutdownStage.store(NativeApplicationShutdownStage::loopReturned,
                                         std::memory_order_release);
    std::fputs("NATIVE_APP_LOOP_RETURN\n", stderr);
    const auto startEventWasHandled = NativeFilePanel::applicationStartEventWasHandled();
    const auto settleEventWasHandled = NativeFilePanel::applicationSettleEventWasHandled();
    const auto stopEventWasPosted = NativeFilePanel::applicationStopEventWasPosted();
    const auto stopEventWasHandled = NativeFilePanel::applicationStopEventWasHandled();
    if (coordinator.failureMessage().empty()) coordinator.verifyFinalState();
    NativeFilePanel::finishTestApplication();
    nativeFileChooserHarnessEnabled = false;
    require(coordinator.completedNormally(), "native chooser application loop stopped before completion");
    require(startEventWasHandled,
            "native chooser AppKit start event was not handled once in the running application loop");
    require(nativeApplicationWasRunningAtStop,
            "native chooser requested shutdown before the AppKit event loop was running");
    require(nativeApplicationSettleEventPosted,
            "native chooser application loop stopped without its AppKit settle event");
    require(settleEventWasHandled,
            "native chooser AppKit settle event was not handled once in the AppKit dispatch path");
    require(stopEventWasPosted,
            "native chooser application loop stopped without its AppKit stop event");
    require(stopEventWasHandled,
            "native chooser AppKit stop event was not handled once in the running application loop");
    if (! coordinator.failureMessage().empty())
        throw std::runtime_error(coordinator.failureMessage());
    std::printf("PASS: %zu real native file-chooser lifecycle regression%s.\n",
                coordinator.caseCount(), coordinator.caseCount() == 1 ? "" : "s");
    std::fputs("NATIVE_TEST_RETURN\n", stderr);
    return 0;
}
#endif
