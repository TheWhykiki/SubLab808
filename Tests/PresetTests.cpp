#include "PluginProcessor.h"
#include "FactoryBank.h"
#include <cstdio>
#include <set>
#include <stdexcept>

#if PRESET_TEST_SUBLAB
using Processor = SubLab808Processor;
#else
using Processor = ReverseLabAudioProcessor;
#endif

namespace
{
void require(bool condition, const char* message)
{
    if (! condition) throw std::runtime_error(message);
}
void ok(const juce::Result& result)
{
    if (result.failed()) throw std::runtime_error(result.getErrorMessage().toStdString());
}
void set(Processor& p, const char* id, float value)
{
    auto* parameter = p.parameters.getParameter(id);
    require(parameter != nullptr, "parameter exists");
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}
std::map<juce::String, float> values(Processor& p)
{
    std::map<juce::String, float> result;
    for (const auto& [id, ignored] : factoryBank().front().values)
    {
        juce::ignoreUnused(ignored);
        result[id] = p.parameters.getRawParameterValue(id)->load();
    }
    return result;
}
void sameValues(Processor& p, const std::map<juce::String, float>& expected)
{
    for (const auto& [id, plain] : expected)
        require(std::abs(p.parameters.getParameter(id)->getValue() - p.parameters.getParameter(id)->convertTo0to1(plain)) < 0.00003f,
                "all parameters restored");
}
struct Tempo : juce::AudioPlayHead
{
    double bpm = 120;
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info; info.setBpm(bpm); info.setTimeSignature(TimeSignature{4,4}); info.setIsPlaying(true); return info;
    }
};
juce::Button* findButton(juce::Component& parent, const juce::String& title)
{
    if (auto* button = dynamic_cast<juce::Button*>(&parent); button != nullptr && button->getTitle() == title) return button;
    for (auto* child : parent.getChildren()) if (auto* result = findButton(*child, title)) return result;
    return nullptr;
}
struct TestWindow final : juce::DocumentWindow
{
    explicit TestWindow(juce::AudioProcessorEditor& editor)
        : DocumentWindow("Preset UI Tests", juce::Colour(0xff101820), DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true); setContentNonOwned(&editor, true);
        centreWithSize(getWidth(), getHeight()); setVisible(true); toFront(true);
    }
    ~TestWindow() override { clearContentComponent(); }
    void closeButtonPressed() override { setVisible(false); }
};
void pump()
{
    // Wait for the queued callbacks, not an arbitrary wall-clock delay. Busy CI
    // machines may need more than 60 ms to dispatch a modal completion callback.
    auto serviced = std::make_shared<bool>(false);
    require(juce::MessageManager::callAsync([serviced] { *serviced = true; }), "post UI queue barrier");
    for (int attempt = 0; attempt < 200 && ! *serviced; ++attempt)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    require(*serviced, "UI queue barrier completed");
}
void writePNG(juce::Component& component, const juce::File& file)
{
    const auto image = component.createComponentSnapshot(component.getLocalBounds());
    juce::FileOutputStream stream(file); juce::PNGImageFormat png;
    stream.setPosition(0); stream.truncate();
    require(stream.openedOk() && png.writeImageToStream(image, stream), "write UI preview");
}
juce::AlertWindow* waitForAlert(const juce::String& title)
{
    // A warning scheduled from a modal completion is delivered on a later queue
    // turn than that completion. Wait for that alert, not the first queue barrier.
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        if (auto* alert = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
            alert != nullptr && alert->getName() == title) return alert;
        pump();
    }
    throw std::runtime_error("Expected asynchronous alert did not open");
}
}
int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("whykiki-preset-tests", "", false);
    root.createDirectory();
    const juce::ScopeGuard cleanup { [&] { root.deleteRecursively(); } };
    const auto outputPath = juce::SystemStats::getEnvironmentVariable("WHYKIKI_PRESET_TEST_OUTPUT_DIR", {});
    const auto artifacts = argc > 1 ? juce::File(argv[1]) : outputPath.isNotEmpty() ? juce::File(outputPath) : root.getChildFile("Artifacts");
    artifacts.createDirectory();
    try
    {
        Processor p(root.getChildFile("Library"));
        require(p.getNumPrograms() == 64, "64 factory presets");
        require(p.presets.userPresets().empty(), "new library empty");
        require(! p.presets.storageDirectory().exists(), "constructing plugin does not create files");
        std::set<juce::String> names;
        for (int index = 0; index < 64; ++index)
        {
            p.setCurrentProgram(index);
            require(p.getCurrentProgram() == index, "factory selection applies");
            require(names.insert(p.getProgramName(index).toLowerCase()).second, "unique factory names");
            require(! p.presets.isModified(), "factory baseline clean");
            sameValues(p, factoryBank()[static_cast<size_t>(index)].values);
            for (const auto& [id, v] : factoryBank()[static_cast<size_t>(index)].values)
            {
                const auto range = p.parameters.getParameter(id)->getNormalisableRange();
                require(std::isfinite(v) && v >= range.start && v <= range.end, "factory value in range");
            }
        }
#if !PRESET_TEST_SUBLAB
        p.setCurrentProgram(0); set(p, "seed", 4243.0f);
        require(p.presets.isModified(), "one-step random seed edit marks preset dirty");
#endif
        p.setCurrentProgram(3);
        set(p, "output", -8.7f);
        require(p.presets.isModified(), "manual changes marked dirty");
        const auto original = values(p);
        ok(p.presets.saveAs(juce::String::fromUTF8("My Bass / Raum Ä"), "My Sounds"));
        const auto saved = p.presets.current();
        require(saved.factoryIndex < 0 && ! p.presets.isModified(), "saved user baseline clean");
        require(p.presets.userPresets().size() == 1, "saved file listed");
        require(p.presets.saveAs(juce::String::fromUTF8(" my bass / raum ä "), "X").failed(), "case-insensitive duplicate refused");
        require(p.presets.saveAs("   ", "X").failed(), "empty name refused");
        require(p.presets.saveAs(juce::String::repeatedString("x", 81), "X").failed(), "long name refused");
        ok(p.presets.setFavourite(saved.id, true));
        ok(p.presets.setFavourite("factory-003", true));
        require(p.presets.setFavourite("../escape", true).failed(), "favourite identity cannot escape directory");
        {
            Processor fresh(root.getChildFile("Library"));
            require(fresh.presets.userPresets().size() == 1 && fresh.presets.isFavourite(saved.id), "library and favourites survive new instance");
            ok(fresh.presets.load(saved)); sameValues(fresh, original);
            require(! fresh.presets.isModified(), "loaded user baseline clean");
        }
        p.setCurrentProgram(1); ok(p.presets.load(saved)); sameValues(p, original);
        set(p, "output", -9.3f);
        ok(p.presets.renameCurrent("Renamed Sound"));
        require(p.presets.isModified(), "renaming does not clear unsaved sound edits");
        ok(p.presets.save());
        require(! p.presets.isModified() && p.presets.userPresets()[0].name == "Renamed Sound", "overwrite saves current settings");
        // External instances must not silently overwrite newer edits to the same file.
        Processor competing(root.getChildFile("Library")); ok(competing.presets.load(p.presets.current()));
        set(p, "output", -10.2f); ok(p.presets.save());
        set(competing, "output", -11.4f);
        require(competing.presets.save().failed(), "concurrent stale overwrite prevented");
        ok(p.presets.load(p.presets.current()));
        set(p, "output", -12.1f);
        const auto edited = values(p);
        juce::MemoryBlock project; p.getStateInformation(project);
        Processor restored(root.getChildFile("Library")); restored.setStateInformation(project.getData(), static_cast<int>(project.getSize()));
        sameValues(restored, edited);
        require(restored.presets.isModified() && restored.presets.current().name == "Renamed Sound", "project preserves user identity and dirty edits");
        // Project reopening does not depend on the preset file still being present.
        Processor portable(root.getChildFile("OtherLibrary")); portable.setStateInformation(project.getData(), static_cast<int>(project.getSize()));
        sameValues(portable, edited);
        require(portable.presets.isModified(), "project baseline remains independent of library files");
        const auto exported = root.getChildFile(juce::String::fromUTF8("Export Grüße") + p.presets.extension());
        ok(p.presets.exportCurrent(exported)); require(p.presets.isModified(), "export does not clear unsaved state");
        wk::Preset imported; ok(p.presets.importPreset(exported, imported));
        require(imported.id != saved.id && imported.name == "Renamed Sound (2)", "import uses new identity and resolves duplicate name");
        ok(p.presets.load(imported)); sameValues(p, edited); require(! p.presets.isModified(), "imported snapshot reloads");
        const auto safeState = values(p); const auto countBefore = p.presets.userPresets().size();
        const auto bad = root.getChildFile("invalid" + p.presets.extension());
        const auto originalJSON = juce::JSON::parse(exported);
        for (int variant = 0; variant < 5; ++variant)
        {
            auto json = originalJSON.clone();
            if (variant == 0) json.getDynamicObject()->setProperty("plugin", "AnotherPlugin");
            if (variant == 1) json.getDynamicObject()->setProperty("version", 99);
            if (variant == 2) json["parameters"].getDynamicObject()->removeProperty("output");
            if (variant == 3) json["parameters"].getDynamicObject()->setProperty("output", 999.0);
            if (variant == 4) json["parameters"].getDynamicObject()->setProperty("output", "not a number");
            require(bad.replaceWithText(juce::JSON::toString(json)), "write invalid fixture");
            wk::Preset rejected; require(p.presets.importPreset(bad, rejected).failed(), "bad import rejected");
            sameValues(p, safeState);
            require(p.presets.userPresets().size() == countBefore, "bad import leaves library unchanged");
        }
        const auto blocked = root.getChildFile("NotADirectory"); blocked.replaceWithText("occupied");
        Processor unwritable(blocked); require(unwritable.presets.saveAs("Blocked", "User").failed(), "write error reported");
        require(unwritable.presets.current().factoryIndex == 0, "failed save does not change identity");
        ok(p.presets.deleteCurrent()); sameValues(p, safeState);
        require(p.presets.userPresets().size() == countBefore - 1, "delete removes user file only");
        p.setCurrentProgram(0);
        require(p.presets.current().factoryIndex == 0, "factory load clears user selection");
        require(p.presets.deleteCurrent().failed() && p.presets.renameCurrent("No").failed() && p.presets.save().failed(), "factory protected");
        ok(p.presets.exportCurrent(root.getChildFile("Factory" + p.presets.extension())));
        ok(p.presets.setFavourite(saved.id, false)); require(! p.presets.isFavourite(saved.id), "favourite removal persists");
        {
            Processor renameGuard(root.getChildFile("RenameGuardLibrary"));
            ok(renameGuard.presets.saveAs("Alpha", "User"));
            const auto alphaId = renameGuard.presets.current().id;
            ok(renameGuard.presets.saveAs("Beta", "User"));
            require(renameGuard.presets.renameCurrent("Wrong target", alphaId).failed(), "stale rename dialog cannot rename another preset");
            require(renameGuard.presets.current().name == "Beta", "stale rename preserves current identity");
            const auto savedNames = renameGuard.presets.userPresets();
            require(savedNames.size() == 2 && savedNames[0].name == "Alpha" && savedNames[1].name == "Beta", "stale rename preserves both files");
        }
        {
            Processor guarded(root.getChildFile("GuardedLibrary"));
            ok(guarded.presets.saveAs("Original", "User"));
            const auto originalPreset = guarded.presets.current();
            const auto storedFile = guarded.presets.storageDirectory().getChildFile(originalPreset.id + guarded.presets.extension());
            const auto storedBytes = storedFile.loadFileAsString();
            set(guarded, "output", -7.5f);
            require(guarded.presets.exportCurrent(storedFile).failed(), "export cannot bypass managed Save");
            require(storedFile.loadFileAsString() == storedBytes, "rejected export preserves stored file");
#if JUCE_MAC || JUCE_LINUX
            const auto aliasDirectory = root.getChildFile("LibraryAlias");
            require(guarded.presets.storageDirectory().createSymbolicLink(aliasDirectory, false), "create library symlink fixture");
            require(guarded.presets.exportCurrent(aliasDirectory.getChildFile(storedFile.getFileName())).failed(), "directory symlink cannot bypass managed Save");
            require(storedFile.loadFileAsString() == storedBytes, "rejected symlink export preserves stored bytes");
#endif
            guarded.setCurrentProgram(2);
            require(guarded.presets.exportCurrent(storedFile).failed(), "factory export cannot replace a user preset");
            require(storedFile.loadFileAsString() == storedBytes, "factory export preserves user identity and values");
            ok(guarded.presets.load(originalPreset));
            auto mismatched = juce::JSON::parse(storedBytes);
            mismatched.getDynamicObject()->setProperty("id", juce::Uuid().toString());
            require(storedFile.replaceWithText(juce::JSON::toString(mismatched)), "write mismatched identity fixture");
            const auto safeSound = guarded.presets.currentSound();
            const auto mismatchedBytes = storedFile.loadFileAsString();
            require(guarded.presets.load(originalPreset).failed(), "load rejects mismatched stored identity");
            require(guarded.presets.save().failed(), "save rejects mismatched stored identity");
            require(guarded.presets.renameCurrent("Wrong").failed(), "rename rejects mismatched stored identity");
            require(guarded.presets.isCurrentSound(safeSound), "invalid library entry preserves current sound");
            require(storedFile.loadFileAsString() == mismatchedBytes, "invalid library entry preserved for recovery");
            wk::Preset recovered; ok(guarded.presets.importPreset(storedFile, recovered));
            require(recovered.id != originalPreset.id, "mismatched entry recoverable by import");
            set(guarded, "output", -6.4f);
            require(! guarded.presets.isCurrentSound(safeSound), "sound guard detects parameter changes within same preset");
        }
        std::printf("PASS: 64 recipes; save/load/rename/delete/import/export/favourites; project recall; invalid files; conflicts; write errors.\n");

        // Render every actual recipe at several tempi/pitches. Check silence, non-finite
        // samples and peaks, while recording RMS and fingerprints for audible diversity.
        juce::String csv("program,name,scenario,peak,rms,hash\n");
        std::set<juce::String> fingerprints;
        Tempo tempo;
        for (int index = 0; index < 64; ++index)
        {
            juce::String combined;
            for (int scenario = 0; scenario < 3; ++scenario)
            {
                Processor render(root.getChildFile("RenderLibrary"));
                tempo.bpm = 60.0 + scenario * 60.0;
                render.setPlayHead(&tempo); render.setCurrentProgram(index);
                constexpr double sampleRate = 48000;
                render.prepareToPlay(sampleRate, 256);
#if PRESET_TEST_SUBLAB
                const int total = 4 * 48000;
#else
                const int total = 18 * 48000;
#endif
                double squares = 0; float peak = 0; std::uint64_t hash = 1469598103934665603ull;
                juce::AudioBuffer<float> audio(2, 256); juce::MidiBuffer midi;
                for (int position = 0; position < total; position += 256)
                {
                    audio.clear(); midi.clear();
#if PRESET_TEST_SUBLAB
                    if (position == 0) midi.addEvent(juce::MidiMessage::noteOn(1, 28 + scenario * 12, static_cast<juce::uint8>(80 + scenario * 23)), 0);
                    if (position == 48000 / 256 * 256) midi.addEvent(juce::MidiMessage::noteOff(1, 28 + scenario * 12), 0);
#else
                    for (int i = 0; i < 256; ++i)
                    {
                        const double t = static_cast<double>(position + i) / sampleRate;
                        for (int channel = 0; channel < 2; ++channel)
                        {
                            const double env = scenario == 0 ? std::exp(-std::fmod(t, .5) * 28.0) : scenario == 1 ? .7 + .3 * std::sin(t * 3.1) : 1.0;
                            const double signal = .16 * std::sin(t * (220 + 37 * channel) * juce::MathConstants<double>::twoPi)
                                + .09 * std::sin(t * (730 + 123 * channel) * juce::MathConstants<double>::twoPi);
                            audio.setSample(channel, i, static_cast<float>(signal * env));
                        }
                    }
#endif
                    render.processBlock(audio, midi);
                    for (int channel = 0; channel < 2; ++channel) for (int i = 0; i < 256; ++i)
                    {
                        const float sample = audio.getSample(channel, i);
                        require(std::isfinite(sample), "finite rendered audio");
                        peak = std::max(peak, std::abs(sample)); squares += static_cast<double>(sample) * sample;
                        if (channel == 0 && i % 32 == 0) { hash ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(sample * 1000000)); hash *= 1099511628211ull; }
                    }
                }
                if (peak < .00001f || peak > 1.001f)
                {
                    std::fprintf(stderr, "render failure: %s scenario %d peak %f\n", render.getProgramName(index).toRawUTF8(), scenario, peak);
                    require(false, "audible output within peak bound");
                }
                const auto digest = juce::String::toHexString(static_cast<juce::int64>(hash)); combined += digest;
                csv += juce::String(index + 1) + ",\"" + render.getProgramName(index) + "\"," + juce::String(scenario) + "," + juce::String(peak, 6)
                    + "," + juce::String(std::sqrt(squares / (total * 2)), 6) + "," + digest + "\n";
                render.releaseResources(); render.setPlayHead(nullptr);
            }
            require(fingerprints.insert(combined).second, "no identical rendered factory recipes");
        }
        require(artifacts.getChildFile("preset-audio-report.csv").replaceWithText(csv), "write audio report");
        std::printf("PASS: 192 audio renders, all finite/audible/within peak limit; 64 distinct fingerprints.\n");
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
#if PRESET_TEST_SUBLAB
        const std::array<juce::Point<int>, 3> sizes {{{820,430},{860,520},{1100,680}}};
#else
        const std::array<juce::Point<int>, 3> sizes {{{720,460},{900,610},{1440,920}}};
#endif
        for (const auto size : sizes)
        {
            editor->setSize(size.x,size.y);
            writePNG(*editor, artifacts.getChildFile("editor-" + juce::String(size.x) + "x" + juce::String(size.y) + ".png"));
        }
        editor->setSize(sizes[1].x, sizes[1].y);
        require(! juce::Desktop::getInstance().getDisplays().displays.isEmpty(), "UI tests require a desktop display (use xvfb-run on Linux)");
        TestWindow window(*editor); pump();
        std::printf("UI: opening Save As\n");
        auto* saveAsButton = findButton(*editor, "Save preset as");
        require(saveAsButton != nullptr, "Save As button visible in editor");
        saveAsButton->onClick();
        auto* dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr && dialog->getTextEditor("name") != nullptr, "Save As dialog opens");
        dialog->getTextEditor("name")->setText("UI Saved Sound");
        require(dialog->getTextEditor("category") != nullptr, "Save As category editor exists");
        dialog->getTextEditor("category")->setText("UI Tests");
        writePNG(*dialog, artifacts.getChildFile("save-dialog.png"));
        std::printf("UI: confirming Save As\n");
        dialog->exitModalState(1); pump();
        require(p.presets.current().name == "UI Saved Sound", "Save As dialog callback writes preset");
        set(p, "output", -13.2f);
        auto* saveButton = findButton(*editor, "Save preset"); require(saveButton != nullptr, "Save button exists");
        std::printf("UI: saving edits\n");
        saveButton->onClick(); require(! p.presets.isModified(), "Save button writes edits");
        auto* nextButton = findButton(*editor, "Next preset"); require(nextButton != nullptr, "Next button exists");
        set(p, "output", -14.6f); const auto beforeCancel = values(p);
        std::printf("UI: dirty navigation\n");
        nextButton->onClick(); pump();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "dirty navigation prompts before replacing sound");
        dialog->exitModalState(0); pump(); sameValues(p, beforeCancel);
        require(p.presets.isModified(), "cancel preserves dirty sound");
        std::printf("UI: dirty navigation\n");
        nextButton->onClick(); pump();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "dirty navigation prompts again");
        dialog->exitModalState(2); pump(); require(! p.presets.isModified(), "discard then load works");
        std::printf("UI: protecting asynchronous dialogs from DAW changes\n");
        set(p, "output", -14.6f);
        nextButton->onClick(); pump();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "dirty dialog opens for stale action test");
        p.setCurrentProgram(5); set(p, "output", -12.4f);
        const auto hostSound = p.presets.currentSound();
        dialog->exitModalState(2); pump();
        require(p.presets.isCurrentSound(hostSound), "stale Discard cannot replace new DAW sound");
        dialog = waitForAlert("Preset library");
        dialog->exitModalState(0); pump();
        const auto usersBeforeStaleSave = p.presets.userPresets().size();
        saveAsButton->onClick();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "Save As opens for stale action test");
        dialog->getTextEditor("name")->setText("Should not save");
        set(p, "output", -11.1f);
        const auto automatedSound = p.presets.currentSound();
        dialog->exitModalState(1); pump();
        require(p.presets.userPresets().size() == usersBeforeStaleSave && p.presets.isCurrentSound(automatedSound), "stale Save As preserves library and automated sound");
        dialog = waitForAlert("Preset library");
        dialog->exitModalState(0); pump();
        p.setCurrentProgram(3);
        auto* browseButton = findButton(*editor, "Browse presets"); require(browseButton != nullptr, "Browser button exists");
        std::printf("UI: browser\n");
        browseButton->onClick(); pump();
        auto* callout = dynamic_cast<juce::CallOutBox*>(juce::Component::getCurrentlyModalComponent());
        require(callout != nullptr, "Preset browser opens");
        writePNG(*callout, artifacts.getChildFile("preset-browser.png"));
        juce::Component* browser = nullptr;
        for (auto* child : callout->getChildren()) if (child->getWidth() == 560) browser = child;
        require(browser != nullptr, "Browser content exists");
        juce::Component::SafePointer<juce::TextEditor> search;
        juce::Component::SafePointer<juce::ListBox> list;
        for (auto* child : browser->getChildren())
        {
            if (auto* input = dynamic_cast<juce::TextEditor*>(child)) search = input;
            if (auto* rows = dynamic_cast<juce::ListBox*>(child)) list = rows;
        }
        require(search != nullptr && list != nullptr && list->getListBoxModel() != nullptr && list->getListBoxModel()->getNumRows() >= 64, "Browser lists full bank");
        search->setText("UI Saved Sound", false); search->onTextChange(); require(list != nullptr && list->getListBoxModel() != nullptr && list->getListBoxModel()->getNumRows() == 1, "Browser search filters user preset");
        require(search != nullptr, "Browser search still open");
        search->setText("no such preset 917239", false); search->onTextChange(); require(list != nullptr && list->getListBoxModel() != nullptr && list->getListBoxModel()->getNumRows() == 0, "Browser empty search state");
        auto* clearFilters = findButton(*browser, "Reset preset filters");
        require(clearFilters != nullptr && clearFilters->isEnabled(), "Clear filters available after empty search");
        clearFilters->onClick();
        require(search->getText().isEmpty() && list->getListBoxModel()->getNumRows() >= 64 && ! clearFilters->isEnabled(), "Clear restores complete bank");
        juce::ComboBox* source = nullptr;
        juce::ComboBox* category = nullptr;
        juce::Label* count = nullptr;
        for (auto* child : browser->getChildren())
        {
            if (child->getTitle() == "Preset source") source = dynamic_cast<juce::ComboBox*>(child);
            if (child->getTitle() == "Preset category") category = dynamic_cast<juce::ComboBox*>(child);
            if (child->getTitle() == "Preset results") count = dynamic_cast<juce::Label*>(child);
        }
        require(source != nullptr && category != nullptr && count != nullptr, "browser filters and result count visible");
        source->setSelectedId(4, juce::dontSendNotification); source->onChange();
        require(list->getListBoxModel()->getNumRows() == 1 && count->getText().startsWith("1 of "), "favourites filter and count agree");
        writePNG(*callout, artifacts.getChildFile("preset-favourites.png"));
        category->setSelectedId(2, juce::dontSendNotification); category->onChange();
        search->setText("missing", false); search->onTextChange();
        clearFilters->onClick();
        require(source->getSelectedId() == 1 && category->getSelectedId() == 1 && list->getListBoxModel()->getNumRows() >= 64, "Clear resets all three filters together");
        writePNG(*callout, artifacts.getChildFile("preset-browser.png"));
        callout->dismiss(); pump();
        std::printf("PASS: editor sizes, Save As/Save UI, unsaved Cancel/Discard, browser and search.\n");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n", error.what()); return 1; }
}
