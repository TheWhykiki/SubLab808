// Included inside PresetTests.cpp's private test namespace so this focused suite
// reuses the real editor/state/library/reopen assertions without exposing hooks
// in product code. Cocoa observation lives in a separate macOS-only .mm file.
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
std::unique_ptr<NativeFilePanel> waitForNativeFilePanel(bool importing)
{
    const auto started = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - started < 10000.0)
    {
        if (auto panel = NativeFilePanel::findVisible(importing, importing ? "Import preset" : "Export current sound"))
        {
            require(panel->hasDelegate(), "native file panel has its real JUCE delegate before teardown");
            require(NativeFilePanel::visibleCount() == 1, "exactly one native file panel belongs to this test process");
            return panel;
        }
        require(juce::MessageManager::getInstance()->runDispatchLoopUntil(10),
                "message dispatch stopped while waiting for native file panel");
    }
    throw std::runtime_error("NATIVE_PANEL_SETUP_TIMEOUT: real NSSavePanel/NSOpenPanel did not become visible");
}
void checkNativeFileChooserLifecycle(const juce::File& root, bool importing, OwnerAction action)
{
    std::printf("NATIVE_CHOOSER_BEGIN: %s / %s\n", importing ? "Import" : "Export", actionName(action));
    requireNoModals();
    require(NativeFilePanel::visibleCount() == 0, "test process starts without a native file panel");
    ok(root.createDirectory());
    const auto external = root.getChildFile("External");
    ok(external.createDirectory());
    Processor processor(root.getChildFile("Library"));
    const auto input = external.getChildFile("Input" + processor.presets.extension());
    const auto output = external.getChildFile("MustNotBeWritten" + processor.presets.extension());
    ok(processor.presets.exportCurrent(input));
    require(input.existsAsFile() && ! output.exists(), "fixture input exists and export destination does not");
    LifecycleEditor owner(processor);
    juce::Process::makeForegroundProcess();
    owner.window->toFront(true);
    const auto activationStarted = juce::Time::getMillisecondCounterHiRes();
    while (! juce::Process::isForegroundProcess() || ! owner.window->isActiveWindow())
    {
        if (juce::Time::getMillisecondCounterHiRes() - activationStarted >= 10000.0)
            throw std::runtime_error("NATIVE_PANEL_SETUP_TIMEOUT: the test process/window did not become active");
        // Desktop runners may briefly reactivate their controller immediately
        // after launching this process. Keep requesting focus until AppKit has
        // delivered activation to our real test window.
        juce::Process::makeForegroundProcess();
        require(juce::MessageManager::getInstance()->runDispatchLoopUntil(10), "activate native test window");
    }
    const LifecycleSnapshot stateBefore(processor);
    const NativeFixtureSnapshot filesBefore(root);

    // Execute the product's management-menu result: do not construct a substitute
    // FileChooser, fake its completion, or accept a non-native fallback as PASS.
    chooseManagement(*owner.editor, importing ? 3 : 4);
    OwnedModalCleanup nativeModal { juce::Component::getCurrentlyModalComponent() };
    require(nativeModal.component != nullptr
            && dynamic_cast<juce::AlertWindow*>(nativeModal.component.getComponent()) == nullptr
            && dynamic_cast<juce::CallOutBox*>(nativeModal.component.getComponent()) == nullptr,
            "the product created a real file-chooser modal");
    auto panel = waitForNativeFilePanel(importing);
    const auto nativeClass = panel->className();
    panel->useFixtureLocation(external.getFullPathName().toStdString(),
                              (importing ? input : output).getFileName().toStdString());
    require(nativeModal.component->isCurrentlyModal() && panel->isVisible(),
            "native panel and JUCE modal are both active before owner transition");
    stateBefore.unchanged(processor);
    filesBefore.unchanged(root);

    std::printf("NATIVE_CHOOSER_OWNER_ACTION_BEGIN: %s / %s\n",
                importing ? "Import" : "Export", actionName(action));
    owner.apply(action);
    std::printf("NATIVE_CHOOSER_OWNER_ACTION_END: %s / %s\n",
                importing ? "Import" : "Export", actionName(action));
    if (action == OwnerAction::destroy || action == OwnerAction::hideThenDestroy)
    {
        require(nativeModal.component == nullptr && ! panel->isVisible() && ! panel->hasDelegate(),
                "editor destruction does not leave plugin code or a native chooser pending past owner lifetime");
    }
    else
    {
        require(nativeModal.component != nullptr && nativeModal.component->isCurrentlyModal()
                    && panel->isVisible() && panel->hasDelegate(),
                "hide/detach returns before non-reentrant native chooser teardown runs on a later timer event");
    }
    waitForDeletion(nativeModal.component, "native file-chooser modal");
    const auto started = juce::Time::getMillisecondCounterHiRes();
    while (panel->isVisible() || panel->hasDelegate())
    {
        if (juce::Time::getMillisecondCounterHiRes() - started >= 10000.0)
            throw std::runtime_error("NATIVE_PANEL_TEARDOWN_TIMEOUT: panel remains visible or retains its JUCE delegate");
        require(juce::MessageManager::getInstance()->runDispatchLoopUntil(10),
                "message dispatch stopped before native panel teardown");
    }
    pump();
    requireNoModals();
    require(NativeFilePanel::visibleCount() == 0, "no native file panel remains visible after owner transition");
    stateBefore.unchanged(processor);
    filesBefore.unchanged(root);
    require(! output.exists(), "cancelled export never creates its target");
    owner.reopen(action);
    stateBefore.unchanged(processor);
    checkReopenedControls(owner);
    filesBefore.unchanged(root);
    std::printf("PASS: native %s / %s [%s]: real panel closed, delegate cleared, modal deleted, state/files unchanged, editor interactive.\n",
                importing ? "Import" : "Export", actionName(action), nativeClass.c_str());
}
void checkNativeFileChooserLifecycles(const juce::File& root, const juce::String& requestedCase = {})
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(), "native chooser tests run on the message thread");
    require(! juce::Desktop::getInstance().getDisplays().displays.isEmpty(),
            "native chooser tests require an active macOS desktop session");
    require(juce::FileChooser::isPlatformDialogAvailable(), "native chooser platform implementation is available");
    NativeFilePanel::prepareTestApplication();
    int executed = 0;
    for (const bool importing : { true, false })
        for (const auto action : { OwnerAction::hideAncestor, OwnerAction::detach,
                                   OwnerAction::destroy, OwnerAction::hideThenDestroy })
        {
            const auto caseName = juce::String(importing ? "import-" : "export-") + actionName(action);
            if (requestedCase.isNotEmpty() && requestedCase != caseName) continue;
            checkNativeFileChooserLifecycle(root.getChildFile(juce::String(importing ? "Import-" : "Export-") + actionName(action)),
                                            importing, action);
            ++executed;
        }
    require(executed > 0, "WHYKIKI_PRESET_TEST_NATIVE_CASE names a supported native chooser case");
    std::printf("PASS: %d real native file-chooser lifecycle regression%s.\n",
                executed, executed == 1 ? "" : "s");
}
#endif
