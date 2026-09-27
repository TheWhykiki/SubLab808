// Shared deterministic persistence/control-thread regressions. Scheduling hooks
// exist only in the preset-test target (PRESET_TEST_SUBLAB is defined there).
juce::MemoryBlock saveRaceState(Processor& processor)
{
    juce::MemoryBlock bytes;
    processor.getStateInformation(bytes);
    return bytes;
}
juce::ValueTree saveRaceTree(const juce::MemoryBlock& bytes)
{
    auto xml = juce::AudioProcessor::getXmlFromBinary(bytes.getData(), static_cast<int>(bytes.getSize()));
    require(xml != nullptr, "save/restore fixture contains binary XML");
    return juce::ValueTree::fromXml(*xml);
}
juce::MemoryBlock saveRaceBytes(const juce::ValueTree& state)
{
    juce::MemoryBlock bytes;
    juce::AudioProcessor::copyXmlToBinary(*state.createXml(), bytes);
    return bytes;
}
void requireSaveRaceState(Processor& processor, const juce::MemoryBlock& expectedBytes)
{
    const auto expected = saveRaceTree(expectedBytes);
    const auto actual = saveRaceTree(saveRaceState(processor));
    require(actual.getType() == expected.getType(), "restored state schema is unchanged");
    for (int i = 0; i < expected.getNumProperties(); ++i)
    {
        const auto property = expected.getPropertyName(i);
        require(actual[property] == expected[property], "restored root metadata is unchanged");
    }
    const auto expectedSelection = expected.getChildWithName("WkPresetSelection");
    require(actual.getChildWithName("WkPresetSelection").isEquivalentTo(expectedSelection),
            "restored complete preset identity/name/category/description/baseline is unchanged");
    const auto current = processor.presets.current();
    if (expectedSelection.isValid())
    {
        require(current.id == expectedSelection["id"].toString() && current.name == expectedSelection["name"].toString(),
                "live restored selection identity and name match the state");
        require(current.values.size() == factoryBank().front().values.size(), "live restored baseline has every parameter");
        for (const auto& [id, value] : current.values)
            require(std::abs(value - static_cast<float>(expectedSelection.getChildWithProperty("id", id)["value"])) <= 0.0f,
                    "live restored selection baseline is exact");
    }
    int count = 0;
    for (const auto& [id, ignored] : factoryBank().front().values)
    {
        juce::ignoreUnused(ignored);
        const auto* parameter = processor.parameters.getParameter(id);
        const auto expectedChild = expected.getChildWithProperty("id", id);
        const auto actualChild = actual.getChildWithProperty("id", id);
        require(parameter != nullptr && expectedChild.hasProperty("value") && actualChild.hasProperty("value"),
                "both complete states have each known parameter");
        const auto legal = [parameter](float value) {
            const auto& range = parameter->getNormalisableRange();
            require(std::isfinite(value) && value >= range.start && value <= range.end, "restored parameter is finite and legal");
            return range.snapToLegalValue(value);
        };
        const auto value = legal(static_cast<float>(expectedChild["value"]));
        require(std::abs(legal(parameter->convertFrom0to1(parameter->getValue())) - value) <= 0.0f,
                "all live authoritative values equal restored Y");
        require(std::abs(legal(static_cast<float>(actualChild["value"])) - value) <= 0.0f,
                "all serialized values equal restored Y");
        ++count;
    }
    require(count == static_cast<int>(factoryBank().front().values.size()), "all product parameters were independently checked");
}
void restoreSaveRaceWorker(Processor& processor, const juce::MemoryBlock& state,
                           const juce::MemoryBlock* intermediate = nullptr)
{
    std::exception_ptr error;
    std::thread worker([&] {
        try
        {
            if (intermediate != nullptr)
                processor.setStateInformation(intermediate->getData(), static_cast<int>(intermediate->getSize()));
            processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            requireSaveRaceState(processor, state);
        }
        catch (...) { error = std::current_exception(); }
    });
    // A scheduling hook, not a JUCE parameter/host callback; no processor or
    // selection gate is held by the joining thread.
    worker.join();
    if (error) std::rethrow_exception(error);
}
void checkPresetSaveRestore(const juce::File& root)
{
    using Step = wk::PresetLibrary::TestStep;
    unsigned passed = 0;
    for (const bool saveExisting : { false, true })
        for (int identityCase = 0; identityCase < 3; ++identityCase)
        {
            const auto fixture = root.getChildFile(juce::String(saveExisting ? "Save-" : "SaveAs-") + juce::String(identityCase));
            Processor processor(fixture.getChildFile("X"));
            processor.setCurrentProgram(1); set(processor, "output", -8.0f);
            ok(processor.presets.saveAs("Original X", "X category"));
            const auto originalId = processor.presets.current().id;
            set(processor, "output", -9.0f);
            const auto capturedX = processor.presets.currentSound().values;
            Processor source(fixture.getChildFile("Y"));
            source.setCurrentProgram(2); set(source, "output", -17.0f);
            ok(source.presets.saveAs("Restored Y", "Y category"));
            set(source, "output", -18.0f);
            auto restoredTree = saveRaceTree(saveRaceState(source));
            restoredTree.setProperty("editorWidth", 1010, nullptr);
            restoredTree.setProperty("editorHeight", 640, nullptr);
            const auto differentIdState = saveRaceBytes(restoredTree);
            if (identityCase != 0)
                restoredTree.getChildWithName("WkPresetSelection").setProperty("id", originalId, nullptr);
            const auto restoredState = saveRaceBytes(restoredTree);
            bool hookCalled = false;
            processor.presets.testStep = [&](Step step) {
                if (step != Step::fileCommitted || hookCalled) return;
                hookCalled = true;
                restoreSaveRaceWorker(processor, restoredState, identityCase == 2 ? &differentIdState : nullptr);
            };
            bool savedSelectionStillCurrent = true;
            ok(saveExisting ? processor.presets.save(&savedSelectionStillCurrent)
                            : processor.presets.saveAs("Captured X", "X category", &savedSelectionStillCurrent));
            processor.presets.testStep = {};
            require(hookCalled, "restore actually ran after the real file commit");
            require(! savedSelectionStillCurrent, "a superseded successful save forbids the stale afterSave load");
            requireSaveRaceState(processor, restoredState);
            require(processor.presets.isModified(), "restored unsaved Y remains dirty against its own saved baseline");
            const auto disk = processor.presets.userPresets();
            require(disk.size() == (saveExisting ? 1u : 2u), "successful old save keeps its actual preset file");
            bool savedXFound = false;
            for (const auto& preset : disk)
                if (preset.name == (saveExisting ? "Original X" : "Captured X"))
                    savedXFound = preset.values == capturedX;
            require(savedXFound, "successful old save preserves exactly the captured X recipe on disk");
            std::printf("PASS: %s file-commit restore case %d preserves Y selection and captured X file.\n",
                        saveExisting ? "Save" : "Save As", identityCase);
            ++passed;
        }

    for (const bool userPreset : { false, true })
        for (const auto step : { Step::selectionCaptured, Step::stateCaptured })
        {
            const auto fixture = root.getChildFile(juce::String(userPreset ? "UserCapture-" : "FactoryCapture-")
                                                  + juce::String(static_cast<int>(step)));
            Processor processor(fixture.getChildFile("X"));
            processor.setCurrentProgram(1);
            if (userPreset) ok(processor.presets.saveAs("Original X", "X category"));
            Processor source(fixture.getChildFile("Y")); source.setCurrentProgram(2);
            if (userPreset) ok(source.presets.saveAs("Restored Y", "Y category"));
            set(source, "output", -18.0f);
            const auto state = saveRaceState(source);
            const auto expected = source.presets.currentSound();
            bool called = false;
            processor.presets.testStep = [&](Step actualStep) {
                if (actualStep != step || called) return;
                called = true;
                restoreSaveRaceWorker(processor, state);
            };
            const auto captured = processor.presets.currentSound();
            processor.presets.testStep = {};
            require(called && captured.id == expected.id && captured.name == expected.name
                        && captured.category == expected.category && captured.values == expected.values,
                    "optimistic capture retries rather than mixing old metadata and new values");
            std::printf("PASS: %s capture retries restore at step %d.\n", userPreset ? "User" : "Factory", static_cast<int>(step));
            ++passed;
        }

    for (const bool saveExisting : { false, true })
    {
        const auto fixture = root.getChildFile(saveExisting ? "Save-HostCallback" : "SaveAs-HostCallback");
        Processor processor(fixture.getChildFile("X")); processor.setCurrentProgram(1);
        ok(processor.presets.saveAs("Original X", "X category"));
        Processor source(fixture.getChildFile("Y")); source.setCurrentProgram(2);
        ok(source.presets.saveAs("Callback Y", "Y category"));
        const auto state = saveRaceState(source);
        struct RestoreOnDisplay final : juce::AudioProcessorListener
        {
            Processor& processor;
            const juce::MemoryBlock& state;
            bool called = false;
            RestoreOnDisplay(Processor& p, const juce::MemoryBlock& s) : processor(p), state(s) { processor.addListener(this); }
            ~RestoreOnDisplay() override { processor.removeListener(this); }
            void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
            void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override
            {
                if (called) return;
                called = true;
                processor.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            }
        } listener(processor, state);
        bool savedSelectionStillCurrent = true;
        ok(saveExisting ? processor.presets.save(&savedSelectionStillCurrent)
                        : processor.presets.saveAs("Captured X", "X category", &savedSelectionStillCurrent));
        require(listener.called && ! savedSelectionStillCurrent, "restore in changed callback invalidates follow-up load after notification");
        requireSaveRaceState(processor, state);
        std::printf("PASS: %s host callback restore suppresses stale follow-up.\n", saveExisting ? "Save" : "Save As");
        ++passed;
    }

    for (const bool saveExisting : { false, true })
    {
        const auto fixture = root.getChildFile(saveExisting ? "Save-BoundedCapture" : "SaveAs-BoundedCapture");
        Processor processor(fixture); processor.setCurrentProgram(1);
        ok(processor.presets.saveAs("Unchanged disk", "Tests"));
        const auto state = saveRaceState(processor);
        const auto before = processor.presets.userPresets();
        unsigned attempts = 0, writes = 0;
        processor.presets.testStep = [&](Step step) {
            if (step == Step::fileCommitted) ++writes;
            if (step != Step::selectionCaptured) return;
            ++attempts;
            restoreSaveRaceWorker(processor, state); // same ID and state still increments revision
        };
        bool selected = true;
        const auto result = saveExisting ? processor.presets.save(&selected)
                                         : processor.presets.saveAs("Must not write", "Tests", &selected);
        processor.presets.testStep = {};
        require(result.failed() && ! selected && attempts == 8 && writes == 0,
                "continuous state changes stop after eight captures, before any file write");
        const auto after = processor.presets.userPresets();
        require(after.size() == before.size() && after.front().id == before.front().id
                    && after.front().values == before.front().values, "busy capture preserves the existing file");
        std::printf("PASS: %s capture contention is bounded and writes nothing.\n", saveExisting ? "Save" : "Save As");
        ++passed;
    }
    require(passed == 14, "all deterministic Save/Restore groups executed");
    std::puts("PASS: 14 deterministic Save/Restore groups (different ID, same ID, ABA, coherent capture, callback and bounded contention).");
}
