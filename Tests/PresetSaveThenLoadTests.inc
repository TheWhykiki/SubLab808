// Exercise the actual Unsaved -> Save As -> afterSave UI chain. The regular
// successful case is a positive control against simply suppressing every load.
void checkSaveThenLoadOrdering(const juce::File& root)
{
    using Step = wk::PresetLibrary::TestStep;
    for (const bool restoreDuringWrite : { false, true })
    {
        requireNoModals();
        const auto fixture = root.getChildFile(restoreDuringWrite ? "RestoreWins" : "NormalFollowup");
        Processor processor(fixture.getChildFile("X"));
        processor.setCurrentProgram(0);
        set(processor, "output", -13.25f);
        const auto capturedX = processor.presets.currentSound().values;
        require(processor.presets.isModified(), "SaveThenLoad starts with edited factory X");

        Processor source(fixture.getChildFile("Y"));
        source.setCurrentProgram(2);
        set(source, "output", -17.0f);
        ok(source.presets.saveAs("Restored UI Y", "UI category"));
        set(source, "output", -18.0f);
        const auto yState = saveRaceState(source);

        LifecycleEditor owner(processor);
        OwnedModalCleanup unsaved { openLifecycleDialog(*owner.editor, LifecycleDialog::unsaved) };
        closeAlertAndWait(dynamic_cast<juce::AlertWindow*>(unsaved.component.getComponent()), 1,
                          "SaveThenLoad selects real Unsaved Save As action");
        OwnedModalCleanup naming { waitForAlert("Save preset as") };
        auto* dialog = dynamic_cast<juce::AlertWindow*>(naming.component.getComponent());
        require(dialog != nullptr, "SaveThenLoad opens the actual naming dialog");
        dialog->getTextEditor("name")->setText("Captured UI X");
        dialog->getTextEditor("category")->setText("UI category");
        unsigned writes = 0;
        processor.presets.testStep = [&](Step step) {
            if (step != Step::fileCommitted) return;
            ++writes;
            if (restoreDuringWrite) restoreSaveRaceWorker(processor, yState);
        };
        closeAlertAndWait(dialog, 1, "SaveThenLoad confirms the actual naming dialog");
        processor.presets.testStep = {};
        pump();
        requireNoModals();
        require(writes == 1, "actual Save As commits exactly one real preset file");
        const auto disk = processor.presets.userPresets();
        require(disk.size() == 1 && disk.front().name == "Captured UI X" && disk.front().values == capturedX,
                "both SaveThenLoad outcomes retain the exact successful X file");
        if (restoreDuringWrite)
        {
            requireSaveRaceState(processor, yState);
            require(processor.getCurrentProgram() == 2 && processor.presets.isModified(),
                    "newer unsaved User Y survives; stale requested factory1 is never loaded");
        }
        else
        {
            require(processor.getCurrentProgram() == 1 && processor.presets.current().factoryIndex == 1
                        && ! processor.presets.isModified(), "ordinary successful Save As still performs requested factory1 follow-up");
            sameValues(processor, factoryBank()[1].values);
        }
        checkReopenedControls(owner);
        std::printf("PASS: actual SaveThenLoad UI %s; X file retained, dialogs dismissed, editor interactive.\n",
                    restoreDuringWrite ? "suppresses stale follow-up after Restore Y" : "performs normal factory follow-up");
    }
}
