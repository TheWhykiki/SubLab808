#include "PluginProcessor.h"
#include "FactoryBank.h"
#include <cstdio>
#include <set>
#include <stdexcept>
#include <thread>
#include <exception>
#if JUCE_MAC
#include "NativeFilePanel.h"
#endif

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
// setValue() changes the authoritative ranged parameter without notifying APVTS.
// Dirty status must be correct on both sides of that intentional cache lag.
void checkAuthoritativeDirtyState(const juce::File& root)
{
    int failures = 0;
    for (const bool userPreset : { false, true })
    {
        Processor processor(root.getChildFile(userPreset ? "User" : "Factory"));
        processor.setCurrentProgram(0);
        if (userPreset) ok(processor.presets.saveAs("Dirty cache contract", "Tests"));
        require((processor.presets.current().factoryIndex < 0) == userPreset, "dirty fixture has the intended preset identity");
        require(! processor.presets.isModified(), "dirty fixture starts clean");
        auto* parameter = processor.parameters.getParameter("output");
        const auto* raw = processor.parameters.getRawParameterValue("output");
        require(parameter != nullptr && raw != nullptr, "dirty fixture has output parameter and raw cache");
        const auto baselineNormalised = parameter->getValue();
        const auto baseline = parameter->convertFrom0to1(baselineNormalised);
        const auto cachedBaseline = raw->load();
        const auto& range = parameter->getNormalisableRange();
        const auto changed = baseline <= range.start ? range.end : range.start;

        parameter->setValue(parameter->convertTo0to1(changed));
        const auto authoritativeChanged = parameter->convertFrom0to1(parameter->getValue());
        require(std::abs(authoritativeChanged - baseline) > 0.0f && std::abs(raw->load() - cachedBaseline) <= 0.0f,
                "unnotified edit changes ranged value while raw cache stays at baseline");
        const auto dirtyBeforeNotification = processor.presets.isModified();
        if (! dirtyBeforeNotification) ++failures;

        parameter->sendValueChangedMessageToListeners(parameter->getValue());
        require(std::abs(raw->load() - authoritativeChanged) <= 0.0f && processor.presets.isModified(),
                "real edit notification converges raw and ranged values and remains dirty");

        parameter->setValue(baselineNormalised);
        require(std::abs(parameter->convertFrom0to1(parameter->getValue()) - baseline) <= 0.0f
                    && std::abs(raw->load() - authoritativeChanged) <= 0.0f,
                "unnotified reset restores ranged baseline while raw cache stays edited");
        const auto cleanBeforeNotification = ! processor.presets.isModified();
        if (! cleanBeforeNotification) ++failures;

        parameter->sendValueChangedMessageToListeners(parameter->getValue());
        require(std::abs(raw->load() - baseline) <= 0.0f && ! processor.presets.isModified(),
                "real reset notification converges raw and ranged values and remains clean");
        std::printf("%s: %s dirty cache contract: unnotified edit dirty=%d; unnotified reset clean=%d; notified values converge.\n",
                    dirtyBeforeNotification && cleanBeforeNotification ? "PASS" : "FAIL",
                    userPreset ? "user" : "factory", dirtyBeforeNotification ? 1 : 0, cleanBeforeNotification ? 1 : 0);
    }
    require(failures == 0, "dirty status follows authoritative ranged values, not the APVTS notification cache");
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
void sameLegalValues(Processor& p, const std::map<juce::String, float>& expected)
{
    const auto actual = values(p);
    require(actual.size() == expected.size(), "loaded sound contains exactly the expected parameters");
    for (const auto& [id, plain] : expected)
    {
        const auto* parameter = p.parameters.getParameter(id);
        const auto found = actual.find(id);
        require(parameter != nullptr && found != actual.end(), "loaded parameter identity exists");
        const auto& range = parameter->getNormalisableRange();
        const auto legal = [parameter, &range](float value)
        {
            require(std::isfinite(value) && value >= range.start && value <= range.end,
                    "loaded and expected values are finite and in range before canonicalisation");
            return parameter->convertFrom0to1(parameter->convertTo0to1(value));
        };
        // APVTS may retain an approximately-equal raw value during state restore.
        // Compare exact legal values, not serialized floats or a broad epsilon.
        require(std::abs(legal(found->second) - legal(plain)) <= 0.0f, "all loaded parameters equal the exact legal saved values");
    }
}
#if PRESET_TEST_SUBLAB
juce::AudioBuffer<float> renderClickPhrase(Processor& processor)
{
    juce::AudioBuffer<float> result(2, 2048), audio(2, 512);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 16);
    for (int block = 0; block < 4; ++block)
    {
        processor.processBlock(audio, midi); midi.clear();
        for (int channel = 0; channel < 2; ++channel)
            result.copyFrom(channel, block * 512, audio, channel, 0, 512);
    }
    return result;
}
float sampleDifference(const juce::AudioBuffer<float>& first, const juce::AudioBuffer<float>& second)
{
    require(first.getNumChannels() == second.getNumChannels() && first.getNumSamples() == second.getNumSamples(),
            "Click comparison dimensions match");
    float difference = 0.0f;
    for (int channel = 0; channel < first.getNumChannels(); ++channel)
        for (int sample = 0; sample < first.getNumSamples(); ++sample)
        {
            const auto a = first.getSample(channel, sample), b = second.getSample(channel, sample);
            require(std::isfinite(a) && std::isfinite(b), "finite Click contract audio");
            difference = std::max(difference, std::abs(a - b));
        }
    return difference;
}
void checkClickPresetContract(const juce::File& directory)
{
    Processor subject(directory), reference(directory.getSiblingFile("ClickReference"));
    require(subject.parameters.getRawParameterValue("click")->load() > 0.0f, "Click contract uses a non-zero transient");
    ok(subject.presets.saveAs("Click reset contract", "Tests"));
    const auto userPreset = subject.presets.current();
    subject.prepareToPlay(48000.0, 512); reference.prepareToPlay(48000.0, 512);
    const auto first = renderClickPhrase(reference);
    require(sampleDifference(first, renderClickPhrase(subject)) == 0.0f, "initial Click phrases match");

    // Exercise the real library load path, not just setCurrentProgram(). Switch away
    // and back without processing between changes, so the final sound is unchanged
    // and any difference from the uninterrupted reference reveals an unwanted reset.
    ok(subject.presets.load(factoryBank()[1]));
    ok(subject.presets.load(factoryBank()[0]));
    require(subject.presets.current().factoryIndex == 0, "factory load selects factory identity");
    const auto continuing = renderClickPhrase(reference);
    const auto factoryDifference = sampleDifference(continuing, renderClickPhrase(subject));
    const auto continuationDifference = sampleDifference(first, continuing);
    require(continuationDifference > 0.00001f, "normal performance advances Click sequence");
    require(factoryDifference == 0.0f, "factory preset loads preserve the continuing Click sequence");

    ok(subject.presets.load(userPreset));
    require(subject.presets.current().id == userPreset.id && ! subject.presets.isModified(), "user load selects saved baseline");
    const auto userDifference = sampleDifference(first, renderClickPhrase(subject));
    require(userDifference == 0.0f, "user preset load resets Click without prepareToPlay");
    ok(subject.presets.load(userPreset));
    const auto repeatUserDifference = sampleDifference(first, renderClickPhrase(subject));
    require(repeatUserDifference == 0.0f, "reloading the same user preset resets Click again");
    std::printf("PASS: Click preset contract: continuation max difference %.9g; factory/user/repeated-user %.9g / %.9g / %.9g.\n",
                static_cast<double>(continuationDifference), static_cast<double>(factoryDifference),
                static_cast<double>(userDifference), static_cast<double>(repeatUserDifference));
}
#endif
#include "PresetSaveRestoreTests.inc"

#if defined(PRESET_TEST_SUBLAB)
int selectionTreeCount(const juce::ValueTree& state)
{
    int count = 0;
    for (const auto& child : state) if (child.hasType("WkPresetSelection")) ++count;
    return count;
}

void checkActionTokenGuards(const juce::File& root)
{
    using Step = wk::PresetLibrary::TestStep;
    enum class Action { saveAs, rename, deletePreset, exportPreset, loadPreset };
    for (const auto action : { Action::saveAs, Action::rename, Action::deletePreset, Action::exportPreset, Action::loadPreset })
    {
        const auto label = action == Action::saveAs ? "SaveAs" : action == Action::rename ? "Rename"
            : action == Action::deletePreset ? "Delete" : action == Action::exportPreset ? "Export" : "Load";
        const auto fixture = root.getChildFile(label);
        Processor processor(fixture.getChildFile("Library"));
        processor.setCurrentProgram(1); set(processor, "output", -8.0f);
        ok(processor.presets.saveAs("Confirmed X", "Tests"));
        const auto token = processor.presets.currentSoundToken();
        require(token.has_value(), "action token captures a coherent confirmed sound");
        const auto xFile = processor.presets.storageDirectory().getChildFile(token->sound.id + processor.presets.extension());
        juce::MemoryBlock xBytes;
        require(xFile.loadFileAsData(xBytes), "confirmed X file exists before guarded action");

        Processor source(fixture.getChildFile("Source"));
        source.setCurrentProgram(2); set(source, "output", -18.0f);
        ok(source.presets.saveAs("Newer Y", "Tests"));
        set(source, "output", -17.0f);
        auto newer = saveRaceTree(saveRaceState(source));
        // Keep the same identity as X so an ID-only guard would falsely pass.
        newer.getChildWithName("WkPresetSelection").setProperty("id", token->sound.id, nullptr);
        const auto newerState = saveRaceBytes(newer);

        unsigned captures = 0;
        bool restored = false;
        processor.presets.testStep = [&](Step step) {
            if (step != Step::selectionCaptured) return;
            ++captures;
            if (restored) return;
            restored = true;
            restoreSaveRaceWorker(processor, newerState);
        };
        const auto exported = fixture.getChildFile("MustNotExist" + processor.presets.extension());
        juce::Result result = juce::Result::fail("guarded action was not selected");
        if (action == Action::saveAs)
            result = processor.presets.saveAs("Must not save", "Tests", nullptr, &*token);
        else if (action == Action::rename)
            result = processor.presets.renameCurrent("Must not rename", token->sound.id, &*token);
        else if (action == Action::deletePreset)
            result = processor.presets.deleteCurrent(&*token);
        else if (action == Action::exportPreset)
            result = processor.presets.exportCurrent(exported, &*token);
        else
            result = processor.presets.load(factoryBank()[4], &*token);
        processor.presets.testStep = {};

        require(restored && captures == 2 && result.failed(), "newer same-ID sound is detected inside the library action");
        requireSaveRaceState(processor, newerState);
        const auto users = processor.presets.userPresets();
        require(users.size() == 1 && users.front().id == token->sound.id && users.front().name == "Confirmed X",
                "guarded action preserves the confirmed user file and its name");
        juce::MemoryBlock afterBytes;
        require(xFile.loadFileAsData(afterBytes) && afterBytes == xBytes && ! exported.exists(),
                "guarded action preserves exact X bytes and creates no export");
        std::printf("PASS: %s token rejects a restore in the pre-capture window with the same ID and a different sound.\n", label);
    }

    for (const auto action : { Action::rename, Action::deletePreset })
    {
        const auto label = action == Action::rename ? "Rename" : "Delete";
        const auto fixture = root.getChildFile(juce::String(label) + "FinalGuard");
        Processor processor(fixture.getChildFile("Library"));
        processor.setCurrentProgram(1); set(processor, "output", -8.1f);
        ok(processor.presets.saveAs("Final guard X", "Tests"));
        const auto token = processor.presets.currentSoundToken();
        require(token.has_value(), "final file guard captures X");
        const auto xFile = processor.presets.storageDirectory().getChildFile(token->sound.id + processor.presets.extension());
        juce::MemoryBlock xBytes;
        require(xFile.loadFileAsData(xBytes), "final file guard fixture exists");

        Processor source(fixture.getChildFile("Source"));
        source.setCurrentProgram(2); set(source, "output", -17.1f);
        ok(source.presets.saveAs("Final guard Y", "Tests"));
        auto newer = saveRaceTree(saveRaceState(source));
        newer.getChildWithName("WkPresetSelection").setProperty("id", token->sound.id, nullptr);
        const auto newerState = saveRaceBytes(newer);
        unsigned captures = 0;
        processor.presets.testStep = [&](Step step) {
            if (step == Step::selectionCaptured && ++captures == 2)
                restoreSaveRaceWorker(processor, newerState);
        };
        const auto result = action == Action::rename
            ? processor.presets.renameCurrent("Must not rename", token->sound.id, &*token)
            : processor.presets.deleteCurrent(&*token);
        processor.presets.testStep = {};
        juce::MemoryBlock afterBytes;
        require(result.failed() && captures == 3 && xFile.loadFileAsData(afterBytes) && afterBytes == xBytes,
                "final token guard after file lock preserves exact X file");
        requireSaveRaceState(processor, newerState);
        std::printf("PASS: %s revalidates its token after acquiring the file lock and before mutation.\n", label);
    }

    Processor contended(root.getChildFile("CaptureExhaustion"));
    ok(contended.presets.saveAs("Capture exhaustion", "Tests"));
    const auto stableState = saveRaceState(contended);
    const auto stableFile = contended.presets.storageDirectory().getChildFile(
        contended.presets.current().id + contended.presets.extension());
    juce::MemoryBlock stableBytes;
    require(stableFile.loadFileAsData(stableBytes), "capture-exhaustion fixture exists");
    unsigned attempts = 0;
    contended.presets.testStep = [&](Step step) {
        if (step != Step::selectionCaptured) return;
        ++attempts;
        restoreSaveRaceWorker(contended, stableState);
    };
    const auto unavailable = contended.presets.currentSoundToken();
    contended.presets.testStep = {};
    juce::MemoryBlock afterBytes;
    require(! unavailable.has_value() && attempts == 8 && stableFile.loadFileAsData(afterBytes) && afterBytes == stableBytes,
            "token capture is bounded and fails without mutating the preset file");
    requireSaveRaceState(contended, stableState);
    std::puts("PASS: action-token capture exhaustion stops after eight coherent attempts and fails closed.");

    // Save As must return the token of the exact selection it committed. Recapturing
    // after the call would let a newer restore masquerade as the saved sound and
    // authorise the stale follow-up load.
    Processor saved(root.getChildFile("SavedToken"));
    saved.setCurrentProgram(1); set(saved, "output", -8.5f);
    const auto originalToken = saved.presets.currentSoundToken();
    require(originalToken.has_value(), "Save As follow-up starts with a coherent original token");
    bool savedSelectionStillCurrent = false;
    std::optional<wk::PresetLibrary::SoundToken> savedToken;
    ok(saved.presets.saveAs("Exact saved token", "Tests", &savedSelectionStillCurrent, &*originalToken, &savedToken));
    require(savedSelectionStillCurrent && savedToken.has_value()
                && saved.presets.isCurrentSound(*savedToken),
            "Save As returns the exact committed selection token");
    Processor restored(root.getChildFile("SavedTokenSource"));
    restored.setCurrentProgram(2); set(restored, "output", -17.25f);
    ok(restored.presets.saveAs("Newer follow-up Y", "Tests"));
    const auto restoredState = saveRaceState(restored);
    restoreSaveRaceWorker(saved, restoredState);
    const auto staleFollowUp = saved.presets.load(factoryBank()[4], &*savedToken);
    require(staleFollowUp.failed(), "saved token rejects a follow-up after a newer restore");
    requireSaveRaceState(saved, restoredState);
    std::puts("PASS: Save As returns its exact committed token; a later restore cannot be recaptured as follow-up authority.");

    Processor guardedLoad(root.getChildFile("FinalUserLoadGuard"));
    guardedLoad.setCurrentProgram(1); set(guardedLoad, "output", -8.75f);
    ok(guardedLoad.presets.saveAs("Final guard X", "Tests"));
    const auto loadToken = guardedLoad.presets.currentSoundToken();
    require(loadToken.has_value(), "guarded user load starts from a coherent token");
    Processor targetWriter(guardedLoad.presets.storageDirectory());
    targetWriter.setCurrentProgram(4); set(targetWriter, "output", -13.5f);
    ok(targetWriter.presets.saveAs("User load target", "Tests"));
    const auto userTarget = targetWriter.presets.current();
    Processor finalRestore(root.getChildFile("FinalUserLoadRestore"));
    finalRestore.setCurrentProgram(3); set(finalRestore, "output", -18.5f);
    ok(finalRestore.presets.saveAs("Final guard Y", "Tests"));
    const auto finalRestoreState = saveRaceState(finalRestore);
    unsigned loadCaptures = 0;
    guardedLoad.presets.testStep = [&](Step step) {
        if (step == Step::selectionCaptured && ++loadCaptures == 2)
            restoreSaveRaceWorker(guardedLoad, finalRestoreState);
    };
    const auto guardedResult = guardedLoad.presets.load(userTarget, &*loadToken);
    guardedLoad.presets.testStep = {};
    require(guardedResult.failed() && loadCaptures == 3,
            "second user-load guard detects a restore after file/state preparation");
    requireSaveRaceState(guardedLoad, finalRestoreState);
    std::puts("PASS: user Load revalidates its token after file/state preparation and preserves a newer restore.");
}

void checkSelectionStateHardening(const juce::File& root)
{
    Processor source(root.getChildFile("Source"));
    source.setCurrentProgram(3); set(source, "output", -8.25f);
    ok(source.presets.saveAs("Valid embedded selection", "Tests"));
    set(source, "output", -11.5f);
    const auto validState = saveRaceTree(saveRaceState(source));
    const auto validSelection = validState.getChildWithName("WkPresetSelection");
    const auto outputRange = source.parameters.getParameter("output")->getNormalisableRange();
    const auto justAboveOutput = std::nextafter(static_cast<double>(outputRange.end),
                                                std::numeric_limits<double>::infinity());
    const auto justAboveOutputText = juce::String(justAboveOutput, 17);
    require(validSelection.isValid() && selectionTreeCount(validState) == 1,
            "selection hardening starts from one real binary-XML-roundtripped user selection");

    {
        Processor restored(root.getChildFile("Valid"));
        const auto bytes = saveRaceBytes(validState);
        restored.setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
        require(restored.presets.current().id == validSelection["id"].toString()
                    && selectionTreeCount(saveRaceTree(saveRaceState(restored))) == 1,
                "one fully valid embedded selection survives a real binary XML roundtrip");
    }

    for (int variant = 0; variant < 7; ++variant)
    {
        auto malformed = validState.createCopy();
        auto selection = malformed.getChildWithName("WkPresetSelection");
        if (variant == 0)
            malformed.addChild(selection.createCopy(), -1, nullptr);
        else if (variant == 1)
            selection.setProperty("description", juce::String::repeatedString("x", 1025), nullptr);
        else if (variant == 2)
            selection.setProperty("name", "invalid\nname", nullptr);
        else if (variant == 3)
            selection.setProperty("id", "not-a-valid-id", nullptr);
        else if (variant == 4)
            selection.removeChild(0, nullptr);
        else if (variant == 5)
            selection.addChild(selection.getChild(0).createCopy(), -1, nullptr);
        else
            selection.getChildWithProperty("id", "output").setProperty("value", justAboveOutputText, nullptr);

        Processor restored(root.getChildFile("Malformed-" + juce::String(variant)));
        const auto bytes = saveRaceBytes(malformed);
        restored.setStateInformation(bytes.getData(), static_cast<int>(bytes.getSize()));
        const auto canonical = saveRaceTree(saveRaceState(restored));
        require(restored.presets.current().factoryIndex == restored.getCurrentProgram()
                    && selectionTreeCount(canonical) == 0,
                "malformed or duplicate embedded selection is rejected and not re-serialized");
        if (variant == 1)
        {
            ok(restored.presets.saveAs("Recovered after invalid metadata", "Tests"));
            Processor reader(restored.presets.storageDirectory());
            const auto users = reader.presets.userPresets();
            require(users.size() == 1 && users.front().description.length() <= 1024,
                    "Save As cannot write metadata that the same library refuses to read");
        }
    }
    const auto invalidImport = root.getChildFile("OutsideRange" + source.presets.extension());
    ok(source.presets.exportCurrent(invalidImport));
    auto json = juce::JSON::parse(invalidImport);
    require(json.isObject(), "strict-range fixture is valid exported JSON");
    json["parameters"].getDynamicObject()->setProperty("output", justAboveOutput);
    require(invalidImport.replaceWithText(juce::JSON::toString(json)), "write strict-range fixture");
    wk::Preset rejected;
    require(source.presets.importPreset(invalidImport, rejected).failed(),
            "persisted preset cannot use the old out-of-range tolerance");
    std::puts("PASS: embedded selection requires one valid node, bounded metadata and a complete unique baseline; output is canonical.");
}
#endif

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
template <typename ComponentType>
void waitForDeletion(juce::Component::SafePointer<ComponentType> component, const char* description)
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(), "modal teardown waits on message thread");
    const auto started = juce::Time::getMillisecondCounterHiRes();
    constexpr double timeoutMs = 2000.0;
    while (component != nullptr)
    {
        if (juce::Time::getMillisecondCounterHiRes() - started >= timeoutMs)
        {
            std::fprintf(stderr, "TEARDOWN_TIMEOUT: %s still alive after %.0f ms (modal=%d visible=%d activeModals=%d)\n",
                         description, juce::Time::getMillisecondCounterHiRes() - started,
                         component->isCurrentlyModal() ? 1 : 0, component->isVisible() ? 1 : 0,
                         juce::Component::getNumCurrentlyModalComponents());
            throw std::runtime_error(juce::String("Modal component was not destroyed: ").toStdString() + description);
        }
        require(juce::MessageManager::getInstance()->runDispatchLoopUntil(10),
                "message dispatch stopped before modal component destruction");
    }
    std::printf("UI teardown: %s destroyed\n", description);
}
void closeAlertAndWait(juce::AlertWindow* dialog, int result, const char* description)
{
    require(dialog != nullptr, "alert exists before explicit close");
    juce::Component::SafePointer<juce::AlertWindow> lifetime(dialog);
    dialog->exitModalState(result);
    waitForDeletion(lifetime, description);
    pump(); // Preserve dispatch of follow-up alerts/callbacks checked by the caller.
}
void dismissCalloutAndWait(juce::CallOutBox* callout)
{
    require(callout != nullptr, "callout exists before explicit dismissal");
    juce::Component::SafePointer<juce::CallOutBox> lifetime(callout);
    callout->dismiss();
    // dismiss() posts a command; ending the modal state queues another update.
    // A single queue barrier does not establish destruction of this component.
    waitForDeletion(lifetime, "preset browser callout");
    require(juce::Component::getNumCurrentlyModalComponents() == 0,
            "no active modal components remain before editor/JUCE cleanup");
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
enum class OwnerAction { hideAncestor, detach, destroy };
enum class LifecycleDialog { saveAs, rename, browser, unsaved, deletePreset, warning, management };

const char* actionName(OwnerAction action)
{
    switch (action)
    {
        case OwnerAction::hideAncestor: return "ancestor-hide";
        case OwnerAction::detach: return "detach";
        case OwnerAction::destroy: return "destroy";
    }
    throw std::runtime_error("unknown owner action");
}
const char* dialogName(LifecycleDialog dialog)
{
    switch (dialog)
    {
        case LifecycleDialog::saveAs: return "Save As";
        case LifecycleDialog::rename: return "Rename";
        case LifecycleDialog::browser: return "Browser";
        case LifecycleDialog::unsaved: return "Unsaved changes";
        case LifecycleDialog::deletePreset: return "Delete";
        case LifecycleDialog::warning: return "Error warning";
        case LifecycleDialog::management: return "Management menu";
    }
    throw std::runtime_error("unknown lifecycle dialog");
}

// Failure cleanup is deliberately limited to the exact dialog captured by this
// fixture. It is not a product assertion and must never hide a failed lifecycle
// assertion by globally cancelling all modal components.
struct OwnedModalCleanup
{
    juce::Component::SafePointer<juce::Component> component;
    ~OwnedModalCleanup()
    {
        if (component == nullptr) return;
        component->exitModalState(0);
        const auto began = juce::Time::getMillisecondCounterHiRes();
        while (component != nullptr && juce::Time::getMillisecondCounterHiRes() - began < 2000.0)
            if (! juce::MessageManager::getInstance()->runDispatchLoopUntil(10)) break;
        if (component != nullptr) std::fprintf(stderr, "FAILURE_CLEANUP: captured dialog still alive\n");
    }
};

struct LifecycleEditor
{
    Processor& processor;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<TestWindow> window;
    explicit LifecycleEditor(Processor& p) : processor(p), editor(p.createEditor()), window(std::make_unique<TestWindow>(*editor))
    {
        pump();
        require(editor->isShowing(), "lifecycle owner must initially be showing");
    }
    void apply(OwnerAction action)
    {
        switch (action)
        {
            // Do not hide PresetBar directly: real hosts hide an ancestor.
            case OwnerAction::hideAncestor: window->setVisible(false); break;
            case OwnerAction::detach: window->clearContentComponent(); break;
            case OwnerAction::destroy:
                // Destroy while still mounted/visible: detaching first would
                // let the detach path mask a broken destructor cleanup.
                editor.reset();
                break;
        }
        require(editor == nullptr || ! editor->isShowing(), "owner action actually hides/detaches/destroys editor");
    }
    void reopen(OwnerAction action)
    {
        if (action == OwnerAction::destroy) editor.reset(processor.createEditor());
        if (action != OwnerAction::hideAncestor) window->setContentNonOwned(editor.get(), true);
        window->setVisible(true);
        window->toFront(true);
        pump();
        require(editor->isShowing(), "reopened lifecycle owner is showing");
    }
};

struct LifecycleSnapshot
{
    juce::MemoryBlock state;
    bool directoryExists;
    std::set<juce::String> directories;
    std::map<juce::String, juce::MemoryBlock> files;
    explicit LifecycleSnapshot(Processor& processor)
        : directoryExists(processor.presets.storageDirectory().exists())
    {
        processor.getStateInformation(state);
        const auto directory = processor.presets.storageDirectory();
        for (const auto& child : directory.findChildFiles(juce::File::findDirectories, true))
            directories.insert(child.getRelativePathFrom(directory));
        for (const auto& file : directory.findChildFiles(juce::File::findFiles, true))
        {
            juce::MemoryBlock bytes;
            require(file.loadFileAsData(bytes), "read lifecycle library snapshot");
            files.emplace(file.getRelativePathFrom(directory), std::move(bytes));
        }
    }
    void unchanged(Processor& processor) const
    {
        const LifecycleSnapshot actual(processor);
        require(actual.state == state, "owner lifecycle preserves exact processor state and preset selection");
        requireSameLibrary(actual);
    }
    void libraryUnchanged(Processor& processor) const
    {
        requireSameLibrary(LifecycleSnapshot(processor));
    }
    void requireSameLibrary(const LifecycleSnapshot& actual) const
    {
        require(actual.directoryExists == directoryExists && actual.directories == directories && actual.files == files,
                "owner lifecycle preserves library existence, names and exact file bytes");
    }
};

juce::Button& lifecycleButton(juce::Component& editor, const char* title)
{
    auto* result = findButton(editor, title);
    require(result != nullptr && result->isEnabled() && result->isShowing(), "lifecycle button is available");
    return *result;
}
juce::Component* openManagement(juce::Component& editor)
{
    lifecycleButton(editor, "Manage presets").onClick();
    pump();
    auto* menu = juce::Component::getCurrentlyModalComponent();
    require(menu != nullptr && dynamic_cast<juce::AlertWindow*>(menu) == nullptr
            && dynamic_cast<juce::CallOutBox*>(menu) == nullptr, "real management popup opened");
    return menu;
}
void chooseManagement(juce::Component& editor, int item)
{
    auto* menu = openManagement(editor);
    OwnedModalCleanup cleanup { menu };
    menu->exitModalState(item); // The same result delivered by choosing the menu item.
    waitForDeletion(cleanup.component, "management menu selection");
    pump();
}
juce::Component* openLifecycleDialog(juce::Component& editor, LifecycleDialog kind)
{
    switch (kind)
    {
        case LifecycleDialog::saveAs:
            lifecycleButton(editor, "Save preset as").onClick();
            return waitForAlert("Save preset as");
        case LifecycleDialog::rename:
            chooseManagement(editor, 1);
            return waitForAlert("Rename preset");
        case LifecycleDialog::browser:
        {
            lifecycleButton(editor, "Browse presets").onClick();
            pump();
            auto* browser = dynamic_cast<juce::CallOutBox*>(juce::Component::getCurrentlyModalComponent());
            require(browser != nullptr, "real preset browser opened for lifecycle test");
            return browser;
        }
        case LifecycleDialog::unsaved:
            lifecycleButton(editor, "Next preset").onClick();
            return waitForAlert("Unsaved preset changes");
        case LifecycleDialog::deletePreset:
            chooseManagement(editor, 2);
            return waitForAlert("Delete user preset?");
        case LifecycleDialog::warning:
        {
            lifecycleButton(editor, "Save preset as").onClick();
            auto* naming = waitForAlert("Save preset as");
            naming->getTextEditor("name")->setText("");
            closeAlertAndWait(naming, 1, "invalid empty-name submission");
            return waitForAlert("Preset library");
        }
        case LifecycleDialog::management:
            return openManagement(editor);
    }
    throw std::runtime_error("unknown lifecycle dialog");
}
void requireNoModals()
{
    pump();
    require(juce::Component::getNumCurrentlyModalComponents() == 0, "no orphan modal remains after owner lifecycle");
}
void checkReopenedControls(LifecycleEditor& owner)
{
    requireNoModals();
    require(! owner.editor->isCurrentlyBlockedByAnotherModalComponent(), "reopened editor is not modal-blocked");
    // This test host already has zero root AX children before opening a modal.
    // Assert the real control's accessibility, not a false root-child baseline.
    auto& saveAs = lifecycleButton(*owner.editor, "Save preset as");
    require(! saveAs.isCurrentlyBlockedByAnotherModalComponent(), "reopened Save As is not modal-blocked");
    auto* saveAccessibility = saveAs.getAccessibilityHandler();
    require(saveAccessibility != nullptr && saveAccessibility->getCurrentState().isFocusable(),
            "reopened Save As is accessibility-focusable");
    const LifecycleSnapshot beforeInteraction(owner.processor);
    // Go through JUCE's queued Button click path, not just the onClick callback.
    saveAs.triggerClick();
    auto* dialog = waitForAlert("Save preset as");
    closeAlertAndWait(dialog, 0, "reopened Save As Cancel");
    beforeInteraction.unchanged(owner.processor);
#if PRESET_TEST_SUBLAB
    auto& toggle = lifecycleButton(*owner.editor, "One Shot playback mode");
    auto* parameter = owner.processor.parameters.getParameter("oneshot");
#else
    auto& toggle = lifecycleButton(*owner.editor, "FREEZE");
    auto* parameter = owner.processor.parameters.getParameter("freeze");
#endif
    require(parameter != nullptr, "reopened toggle parameter exists");
    require(! toggle.isCurrentlyBlockedByAnotherModalComponent(), "reopened sound control is not modal-blocked");
    auto* controlAccessibility = toggle.getAccessibilityHandler();
    require(controlAccessibility != nullptr && controlAccessibility->getCurrentState().isFocusable(),
            "reopened sound control is accessibility-focusable");
    const auto initial = parameter->getValue();
    toggle.triggerClick(); pump();
    require(std::abs(parameter->getValue() - initial) > 0.5f, "reopened sound-control click updates its actual parameter");
    toggle.triggerClick(); pump();
    require(std::abs(parameter->getValue() - initial) <= 0.0f, "second sound-control click restores its actual parameter");
    requireNoModals();
    // Deliberate control clicks may legitimately set the processor's historical
    // modified flag, even when toggled back. Exact lifecycle state was checked
    // before these clicks; library bytes must remain identical afterwards too.
    beforeInteraction.libraryUnchanged(owner.processor);
}
void prepareLifecyclePreset(Processor& processor, LifecycleDialog kind)
{
    if (kind == LifecycleDialog::rename || kind == LifecycleDialog::deletePreset
        || kind == LifecycleDialog::unsaved || kind == LifecycleDialog::management)
        ok(processor.presets.saveAs("Lifecycle fixture", "Tests"));
    if (kind == LifecycleDialog::unsaved)
    {
        set(processor, "output", -13.25f);
        require(processor.presets.isModified(), "unsaved lifecycle fixture is actually dirty");
    }
}
void checkOwnerLifecycle(const juce::File& root, LifecycleDialog kind, OwnerAction action)
{
    std::printf("LIFECYCLE_BEGIN: %s / %s\n", dialogName(kind), actionName(action));
    requireNoModals();
    Processor processor(root);
    prepareLifecyclePreset(processor, kind);
    LifecycleEditor owner(processor);
    const LifecycleSnapshot before(processor);
    OwnedModalCleanup dialog { openLifecycleDialog(*owner.editor, kind) };
    require(dialog.component->isCurrentlyModal() && juce::Component::getNumCurrentlyModalComponents() == 1,
            "one real owned dialog is modal before lifecycle action");
    owner.apply(action);
    waitForDeletion(dialog.component, dialogName(kind));
    requireNoModals();
    before.unchanged(processor);
    owner.reopen(action);
    before.unchanged(processor);
    checkReopenedControls(owner);
    std::printf("PASS: lifecycle %s / %s: dialog destroyed, modal count zero, reopen interactive, state/library unchanged.\n",
                dialogName(kind), actionName(action));
}
void checkQueuedLifecycleResult(const juce::File& root, LifecycleDialog kind, int result)
{
    std::printf("LIFECYCLE_BEGIN: queued %s result %d / hide-reshow\n", dialogName(kind), result);
    requireNoModals();
    Processor processor(root);
    prepareLifecyclePreset(processor, kind);
    LifecycleEditor owner(processor);
    const LifecycleSnapshot before(processor);
    OwnedModalCleanup dialog { openLifecycleDialog(*owner.editor, kind) };
    if (kind == LifecycleDialog::saveAs || kind == LifecycleDialog::rename)
    {
        auto* alert = dynamic_cast<juce::AlertWindow*>(dialog.component.getComponent());
        require(alert != nullptr && alert->getTextEditor("name") != nullptr, "queued naming dialog has editor");
        alert->getTextEditor("name")->setText("Must not be written");
    }
    dialog.component->exitModalState(result);
    // No dispatch between completion and the host hiding then reshowing the same
    // editor. isShowing() alone in the old callback cannot detect this stale UI.
    owner.apply(OwnerAction::hideAncestor);
    owner.window->setVisible(true);
    owner.window->toFront(true);
    waitForDeletion(dialog.component, "queued old-generation confirmation");
    pump();
    // Capture an incorrectly opened follow-up before asserting, so a RED test
    // still destroys only this fixture's dialog when unwinding.
    OwnedModalCleanup unexpectedFollowup { juce::Component::getCurrentlyModalComponent() };
    requireNoModals();
    before.unchanged(processor);
    checkReopenedControls(owner);
    std::printf("PASS: lifecycle queued %s result %d ignored after hide-reshow; no follow-up, state/library unchanged.\n",
                dialogName(kind), result);
}
void checkInstanceIsolation(const juce::File& root, OwnerAction action)
{
    std::printf("LIFECYCLE_BEGIN: two-instance isolation / %s\n", actionName(action));
    requireNoModals();
    Processor first(root.getChildFile("First")), second(root.getChildFile("Second"));
    LifecycleEditor firstOwner(first), secondOwner(second);
    const LifecycleSnapshot firstBefore(first), secondBefore(second);
    OwnedModalCleanup firstDialog { openLifecycleDialog(*firstOwner.editor, LifecycleDialog::saveAs) };
    OwnedModalCleanup secondDialog { openLifecycleDialog(*secondOwner.editor, LifecycleDialog::saveAs) };
    require(juce::Component::getNumCurrentlyModalComponents() == 2, "both instances have distinct real dialogs");
    firstOwner.apply(action);
    waitForDeletion(firstDialog.component, "first instance dialog");
    pump();
    require(secondDialog.component != nullptr && secondDialog.component->isShowing()
            && secondDialog.component->isCurrentlyModal(), "other instance dialog remains alive, showing and modal");
    require(juce::Component::getNumCurrentlyModalComponents() == 1, "only the other instance modal remains");
    firstBefore.unchanged(first); secondBefore.unchanged(second);
    auto* secondAlert = dynamic_cast<juce::AlertWindow*>(secondDialog.component.getComponent());
    closeAlertAndWait(secondAlert, 0, "second instance explicit Cancel");
    requireNoModals();
    firstOwner.reopen(action);
    firstBefore.unchanged(first); secondBefore.unchanged(second);
    checkReopenedControls(firstOwner);
    checkReopenedControls(secondOwner);
    std::printf("PASS: lifecycle isolation / %s: only own dialog cancelled; both editors reusable; both libraries unchanged.\n",
                actionName(action));
}
enum class ReentrantAction { saveAs, save, rename, deletePreset, factoryLoad, discardLoad, saveThenLoad, userLoad };
const char* reentrantName(ReentrantAction action)
{
    switch (action)
    {
        case ReentrantAction::saveAs: return "Save As";
        case ReentrantAction::save: return "Save";
        case ReentrantAction::rename: return "Rename";
        case ReentrantAction::deletePreset: return "Delete";
        case ReentrantAction::factoryLoad: return "Factory load";
        case ReentrantAction::discardLoad: return "Discard then load";
        case ReentrantAction::saveThenLoad: return "Save As then stale load";
        case ReentrantAction::userLoad: return "Browser user load";
    }
    throw std::runtime_error("unknown reentrant action");
}

// A real synchronous host notification, not an artificial call to a PresetBar
// helper. Only this fixture's editor is destroyed; its processor/library survive.
struct DestroyEditorOnStateChange final : juce::AudioProcessorListener
{
    LifecycleEditor& owner;
    juce::Component::SafePointer<juce::AudioProcessorEditor> lifetime;
    bool armed = false, onMessageThread = false, mountedWhenDestroyed = false;
    int notifications = 0, destructions = 0;

    explicit DestroyEditorOnStateChange(LifecycleEditor& editorOwner)
        : owner(editorOwner), lifetime(editorOwner.editor.get())
    {
        owner.processor.addListener(this);
    }
    ~DestroyEditorOnStateChange() override { owner.processor.removeListener(this); }
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor* processor, const ChangeDetails& details) override
    {
        if (processor != &owner.processor || ! details.nonParameterStateChanged) return;
        ++notifications;
        if (! armed) return;
        armed = false; // Never delete twice if a buggy continuation notifies again.
        onMessageThread = juce::MessageManager::getInstance()->isThisTheMessageThread();
        if (! onMessageThread || owner.editor == nullptr) return;
        mountedWhenDestroyed = owner.editor->isShowing() && owner.editor->getParentComponent() != nullptr;
        ++destructions;
        std::puts("REENTRANCY_HOST: destroying mounted editor inside nonParameterStateChanged");
        owner.editor.reset(); // No preliminary detach that could mask the destructor.
    }
    void requireDestroyed() const
    {
        require(onMessageThread && mountedWhenDestroyed, "host callback destroyed the visible, mounted editor on message thread");
        require(destructions == 1 && lifetime == nullptr && owner.editor == nullptr, "exactly one editor really died inside host notification");
        require(notifications == 1, "no stale continuation performs a second notified library operation");
    }
};
juce::ListBox* findPresetList(juce::Component& parent)
{
    if (auto* list = dynamic_cast<juce::ListBox*>(&parent); list != nullptr && list->getTitle() == "Presets") return list;
    for (auto* child : parent.getChildren()) if (auto* list = findPresetList(*child)) return list;
    return nullptr;
}
void checkReentrantLibraryAction(const juce::File& root, ReentrantAction action)
{
    std::printf("REENTRANCY_BEGIN: %s\n", reentrantName(action));
    requireNoModals();
    Processor processor(root);
    wk::Preset saved;
    const bool existingUser = action == ReentrantAction::save || action == ReentrantAction::rename
        || action == ReentrantAction::deletePreset || action == ReentrantAction::userLoad;
    if (existingUser)
    {
        processor.setCurrentProgram(3);
        set(processor, "output", -9.75f);
        ok(processor.presets.saveAs("Reentrant original", "Tests"));
        saved = processor.presets.current();
    }
    if (action == ReentrantAction::userLoad) processor.setCurrentProgram(0);
    if (action == ReentrantAction::saveAs || action == ReentrantAction::save
        || action == ReentrantAction::discardLoad || action == ReentrantAction::saveThenLoad)
        set(processor, "output", -13.25f);

    LifecycleEditor owner(processor);
    DestroyEditorOnStateChange listener(owner);
    const LifecycleSnapshot before(processor);
    const auto sourceSound = values(processor);
    // APVTS raw floats and its state-tree capture can differ by one ULP. Keep
    // separate exact oracles for raw preservation and the recipe being saved.
    const auto sourceCapture = processor.presets.currentSound().values;
    OwnedModalCleanup modal;
    juce::Button* directAction = nullptr;
    if (action == ReentrantAction::saveAs || action == ReentrantAction::rename)
    {
        modal.component = openLifecycleDialog(*owner.editor,
            action == ReentrantAction::rename ? LifecycleDialog::rename : LifecycleDialog::saveAs);
        auto* dialog = dynamic_cast<juce::AlertWindow*>(modal.component.getComponent());
        require(dialog != nullptr, "reentrant naming dialog opened");
        dialog->getTextEditor("name")->setText("Reentrant confirmed");
    }
    else if (action == ReentrantAction::deletePreset)
        modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::deletePreset);
    else if (action == ReentrantAction::discardLoad || action == ReentrantAction::saveThenLoad)
    {
        modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::unsaved);
        if (action == ReentrantAction::saveThenLoad)
        {
            closeAlertAndWait(dynamic_cast<juce::AlertWindow*>(modal.component.getComponent()), 1,
                              "reentrant Unsaved Save As selection");
            modal.component = waitForAlert("Save preset as");
            dynamic_cast<juce::AlertWindow*>(modal.component.getComponent())
                ->getTextEditor("name")->setText("Reentrant confirmed");
        }
    }
    else if (action == ReentrantAction::userLoad)
    {
        modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::browser);
        auto* list = findPresetList(*modal.component);
        require(list != nullptr && list->getListBoxModel() != nullptr
                && list->getListBoxModel()->getNumRows() == 65, "user-load browser has 64 factory rows and one fixture user");
        list->selectRow(64); // The one user follows the unchanged 64-entry factory bank.
        directAction = &lifecycleButton(*modal.component, "Load selected preset");
    }
    else
        directAction = &lifecycleButton(*owner.editor, action == ReentrantAction::save ? "Save preset" : "Next preset");

    require(listener.notifications == 0 && listener.lifetime != nullptr, "fixture setup did not trigger the armed mutation");
    listener.armed = true;
    if (directAction != nullptr)
    {
        // Use the actual UI callback. A copy must remain alive when the callback
        // destroys the Button containing the original std::function.
        const auto click = directAction->onClick;
        require(static_cast<bool>(click), "reentrant action has a real UI callback");
        click();
    }
    else
    {
        modal.component->exitModalState(action == ReentrantAction::discardLoad ? 2 : 1);
    }
    if (modal.component != nullptr) waitForDeletion(modal.component, reentrantName(action));
    pump();
    OwnedModalCleanup unexpectedFollowup { juce::Component::getCurrentlyModalComponent() };
    listener.requireDestroyed();
    requireNoModals();

    // The explicit operation is allowed to finish. Rejecting every write here
    // would conceal data loss rather than test safe editor lifetime handling.
    if (action == ReentrantAction::saveAs || action == ReentrantAction::saveThenLoad)
    {
        const auto users = processor.presets.userPresets();
        require(users.size() == 1 && users.front().name == "Reentrant confirmed", "confirmed Save As writes exactly one named user preset");
        require(processor.presets.current().id == users.front().id && ! processor.presets.isModified(),
                "confirmed Save As remains selected; stale afterSave must not load the requested next preset");
        require(values(processor) == sourceSound, "confirmed Save As preserves exact raw parameter values");
        require(users.front().values == sourceCapture, "confirmed Save As writes the exact pre-action captured recipe");
    }
    else if (action == ReentrantAction::save)
    {
        const auto users = processor.presets.userPresets();
        require(users.size() == 1 && users.front().id == saved.id && users.front().name == saved.name,
                "confirmed Save retains the existing preset identity and name");
        require(values(processor) == sourceSound, "confirmed Save preserves exact raw parameter values");
        require(users.front().values == sourceCapture && ! processor.presets.isModified(),
                "confirmed Save commits the exact pre-action captured recipe despite editor destruction");
    }
    else if (action == ReentrantAction::rename)
    {
        const auto users = processor.presets.userPresets();
        require(users.size() == 1 && users.front().id == saved.id && users.front().name == "Reentrant confirmed"
                && processor.presets.current().id == saved.id && processor.presets.current().name == "Reentrant confirmed",
                "confirmed Rename updates exactly the requested existing identity");
        require(values(processor) == sourceSound && users.front().values == saved.values, "Rename preserves sound and stored values");
    }
    else if (action == ReentrantAction::deletePreset)
    {
        require(processor.presets.userPresets().empty()
                && ! root.getChildFile(saved.id + processor.presets.extension()).exists(),
                "confirmed Delete removes exactly the fixture preset file");
        require(values(processor) == sourceSound && processor.presets.current().id == saved.id,
                "Delete preserves the project sound and recoverable selection");
    }
    else
    {
        before.libraryUnchanged(processor);
        if (action == ReentrantAction::userLoad)
        {
            require(processor.presets.current().id == saved.id, "confirmed user Load selects the exact saved identity");
            sameLegalValues(processor, saved.values);
        }
        else
        {
            require(processor.presets.current().factoryIndex == 1 && processor.getCurrentProgram() == 1,
                    "confirmed factory Load selects the requested next program");
            sameValues(processor, factoryBank()[1].values);
        }
        require(! processor.presets.isModified(), "confirmed Load establishes a clean baseline");
    }
    const LifecycleSnapshot afterOperation(processor);
    pump();
    requireNoModals();
    afterOperation.unchanged(processor);
    owner.reopen(OwnerAction::destroy);
    afterOperation.unchanged(processor);
    checkReopenedControls(owner);
    std::printf("PASS: reentrancy %s: one mounted editor destroyed, confirmed operation committed, no stale follow-up/modal, replacement interactive.\n",
                reentrantName(action));
}

#if defined(PRESET_TEST_SUBLAB)
void checkUiActionTokenGuards(const juce::File& root)
{
    using Step = wk::PresetLibrary::TestStep;
    enum class Action { saveAs, rename, deletePreset, discardLoad };
    for (const auto action : { Action::saveAs, Action::rename, Action::deletePreset, Action::discardLoad })
    {
        const auto label = action == Action::saveAs ? "SaveAs" : action == Action::rename ? "Rename"
            : action == Action::deletePreset ? "Delete" : "Discard";
        const auto fixture = root.getChildFile(label);
        Processor processor(fixture.getChildFile("Library"));
        processor.setCurrentProgram(1); set(processor, "output", -8.0f);
        ok(processor.presets.saveAs("UI confirmed X", "Tests"));
        const auto confirmed = processor.presets.current();
        const auto xFile = processor.presets.storageDirectory().getChildFile(confirmed.id + processor.presets.extension());
        juce::MemoryBlock xBytes;
        require(xFile.loadFileAsData(xBytes), "UI token fixture X exists");
        if (action == Action::discardLoad) set(processor, "output", -9.0f);

        Processor source(fixture.getChildFile("Source"));
        source.setCurrentProgram(2); set(source, "output", -18.0f);
        ok(source.presets.saveAs("UI newer Y", "Tests"));
        set(source, "output", -17.0f);
        auto newer = saveRaceTree(saveRaceState(source));
        newer.getChildWithName("WkPresetSelection").setProperty("id", confirmed.id, nullptr);
        const auto newerState = saveRaceBytes(newer);

        LifecycleEditor owner(processor);
        OwnedModalCleanup modal;
        if (action == Action::saveAs)
        {
            modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::saveAs);
            dynamic_cast<juce::AlertWindow*>(modal.component.getComponent())->getTextEditor("name")->setText("Must not save");
        }
        else if (action == Action::rename)
        {
            modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::rename);
            dynamic_cast<juce::AlertWindow*>(modal.component.getComponent())->getTextEditor("name")->setText("Must not rename");
        }
        else if (action == Action::deletePreset)
            modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::deletePreset);
        else
            modal.component = openLifecycleDialog(*owner.editor, LifecycleDialog::unsaved);

        unsigned captures = 0;
        bool restored = false;
        const auto restoreOnCapture = action == Action::discardLoad ? 2u : 1u;
        processor.presets.testStep = [&](Step step) {
            if (step != Step::selectionCaptured || ++captures != restoreOnCapture) return;
            restored = true;
            restoreSaveRaceWorker(processor, newerState);
        };
        closeAlertAndWait(dynamic_cast<juce::AlertWindow*>(modal.component.getComponent()),
                          action == Action::discardLoad ? 2 : 1, "guarded UI action");
        processor.presets.testStep = {};
        require(restored, "newer state lands after the UI check and inside the guarded library action");
        OwnedModalCleanup warning { waitForAlert("Preset library") };
        closeAlertAndWait(dynamic_cast<juce::AlertWindow*>(warning.component.getComponent()), 0, "guarded action warning");
        requireNoModals();
        requireSaveRaceState(processor, newerState);
        const auto users = processor.presets.userPresets();
        juce::MemoryBlock afterBytes;
        require(users.size() == 1 && users.front().id == confirmed.id && users.front().name == "UI confirmed X"
                    && xFile.loadFileAsData(afterBytes) && afterBytes == xBytes,
                "actual UI action preserves the confirmed X file when same-ID Y wins");
        checkReopenedControls(owner);
        std::printf("PASS: actual %s UI path carries its token into the mutation and preserves newer same-ID Y.\n", label);
    }

    Processor contended(root.getChildFile("RenameCaptureExhaustion"));
    ok(contended.presets.saveAs("Rename remains unchanged", "Tests"));
    const auto stableState = saveRaceState(contended);
    const auto stableFile = contended.presets.storageDirectory().getChildFile(
        contended.presets.current().id + contended.presets.extension());
    juce::MemoryBlock stableBytes;
    require(stableFile.loadFileAsData(stableBytes), "UI capture-exhaustion fixture exists");
    LifecycleEditor owner(contended);
    unsigned attempts = 0;
    contended.presets.testStep = [&](Step step) {
        if (step != Step::selectionCaptured) return;
        ++attempts;
        restoreSaveRaceWorker(contended, stableState);
    };
    lifecycleButton(*owner.editor, "Manage presets").onClick();
    auto* warning = waitForAlert("Preset library");
    contended.presets.testStep = {};
    OwnedModalCleanup warningCleanup { warning };
    require(attempts == 8 && dynamic_cast<juce::AlertWindow*>(warning) != nullptr,
            "eight failed captures open only an error, never a Rename dialog with an empty identity");
    closeAlertAndWait(warning, 0, "capture-exhaustion warning");
    requireNoModals();
    juce::MemoryBlock afterBytes;
    require(stableFile.loadFileAsData(afterBytes) && afterBytes == stableBytes
                && contended.presets.current().name == "Rename remains unchanged",
            "capture exhaustion preserves the exact selected file and identity");
    checkReopenedControls(owner);
    std::puts("PASS: actual Rename entry fails closed after eight capture collisions; no empty-ID dialog opens.");
}
#endif
#include "PresetSaveThenLoadTests.inc"

void checkPresetReentrancy(const juce::File& root)
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(), "reentrancy tests run on message thread");
    require(! juce::Desktop::getInstance().getDisplays().displays.isEmpty(),
            "reentrancy tests require a desktop display (use xvfb-run on Linux)");
    for (const auto action : { ReentrantAction::saveAs, ReentrantAction::save, ReentrantAction::rename,
                               ReentrantAction::deletePreset, ReentrantAction::factoryLoad, ReentrantAction::discardLoad,
                               ReentrantAction::saveThenLoad, ReentrantAction::userLoad })
        checkReentrantLibraryAction(root.getChildFile(reentrantName(action)), action);
    std::puts("PASS: 8 synchronous host-callback editor-destruction regressions.");
    checkSaveThenLoadOrdering(root.getChildFile("SaveThenLoadOrdering"));
#if defined(PRESET_TEST_SUBLAB)
    checkUiActionTokenGuards(root.getChildFile("ActionTokenUi"));
#endif
}
void checkPresetDialogLifecycles(const juce::File& root)
{
    require(juce::MessageManager::getInstance()->isThisTheMessageThread(), "lifecycle tests run on message thread");
    require(! juce::Desktop::getInstance().getDisplays().displays.isEmpty(),
            "lifecycle tests require a desktop display (use xvfb-run on Linux)");
    for (const auto kind : { LifecycleDialog::saveAs, LifecycleDialog::rename, LifecycleDialog::browser,
                             LifecycleDialog::unsaved, LifecycleDialog::deletePreset, LifecycleDialog::warning,
                             LifecycleDialog::management })
        for (const auto action : { OwnerAction::hideAncestor, OwnerAction::detach, OwnerAction::destroy })
            checkOwnerLifecycle(root.getChildFile(juce::String(dialogName(kind)) + "-" + actionName(action)), kind, action);
    for (const auto kind : { LifecycleDialog::saveAs, LifecycleDialog::rename, LifecycleDialog::deletePreset })
        checkQueuedLifecycleResult(root.getChildFile(juce::String("Queued-") + dialogName(kind)), kind, 1);
    for (const int result : { 1, 2 })
        checkQueuedLifecycleResult(root.getChildFile("Queued-unsaved-" + juce::String(result)), LifecycleDialog::unsaved, result);
    for (const auto action : { OwnerAction::hideAncestor, OwnerAction::detach, OwnerAction::destroy })
        checkInstanceIsolation(root.getChildFile(juce::String("Isolation-") + actionName(action)), action);
    std::puts("PASS: 29 preset lifecycle regressions (21 owner transitions, 5 queued confirmations, 3 instance-isolation cases).");
}
#include "NativeFileChooserLifecycle.inc"
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
        const auto dirtyOnly = juce::SystemStats::getEnvironmentVariable("WHYKIKI_PRESET_TEST_DIRTY_ONLY", {}) == "1";
        const auto saveRestoreOnly = juce::SystemStats::getEnvironmentVariable("WHYKIKI_PRESET_TEST_SAVE_RESTORE_ONLY", {}) == "1";
        require(! (dirtyOnly && saveRestoreOnly), "DIRTY_ONLY and SAVE_RESTORE_ONLY cannot be combined");
        if (dirtyOnly || saveRestoreOnly)
            for (const auto* mode : { "WHYKIKI_PRESET_TEST_NATIVE_ONLY", "WHYKIKI_PRESET_TEST_REENTRANCY_ONLY",
                                      "WHYKIKI_PRESET_TEST_LIFECYCLE_ONLY" })
                require(juce::SystemStats::getEnvironmentVariable(mode, {}) != "1",
                        "DIRTY_ONLY or SAVE_RESTORE_ONLY cannot be combined with another focused test mode");
        checkAuthoritativeDirtyState(root.getChildFile("AuthoritativeDirty"));
        if (dirtyOnly) return 0;
        checkPresetSaveRestore(root.getChildFile("SaveRestore"));
#if defined(PRESET_TEST_SUBLAB)
        checkActionTokenGuards(root.getChildFile("ActionTokens"));
        checkSelectionStateHardening(root.getChildFile("SelectionState"));
#endif
        if (saveRestoreOnly) return 0;
        if (juce::SystemStats::getEnvironmentVariable("WHYKIKI_PRESET_TEST_NATIVE_ONLY", {}) == "1")
        {
#if JUCE_MAC
            checkNativeFileChooserLifecycles(root.getChildFile("NativeChoosers"));
            return 0;
#else
            throw std::runtime_error("Native file-chooser tests require macOS; a non-native fallback is not accepted.");
#endif
        }
        checkPresetReentrancy(root.getChildFile("Reentrancy"));
        if (juce::SystemStats::getEnvironmentVariable("WHYKIKI_PRESET_TEST_REENTRANCY_ONLY", {}) == "1") return 0;
        checkPresetDialogLifecycles(root.getChildFile("Lifecycles"));
        if (juce::SystemStats::getEnvironmentVariable("WHYKIKI_PRESET_TEST_LIFECYCLE_ONLY", {}) == "1") return 0;
#if JUCE_MAC
        checkNativeFileChooserLifecycles(root.getChildFile("NativeChoosers"));
#endif
#if PRESET_TEST_SUBLAB
        checkClickPresetContract(root.getChildFile("ClickContract"));
#endif
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
        for (int variant = 0; variant < 6; ++variant)
        {
            auto json = originalJSON.clone();
            if (variant == 0) json.getDynamicObject()->setProperty("plugin", "AnotherPlugin");
            if (variant == 1) json.getDynamicObject()->setProperty("version", 99);
            if (variant == 2) json["parameters"].getDynamicObject()->removeProperty("output");
            if (variant == 3) json["parameters"].getDynamicObject()->setProperty("output", 999.0);
            if (variant == 4) json["parameters"].getDynamicObject()->setProperty("output", "not a number");
            if (variant == 5)
            {
                const auto& range = p.parameters.getParameter("output")->getNormalisableRange();
                json["parameters"].getDynamicObject()->setProperty(
                    "output", std::nextafter(static_cast<double>(range.end),
                                              std::numeric_limits<double>::infinity()));
            }
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
        closeAlertAndWait(dialog, 1, "Save As confirmation");
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
        closeAlertAndWait(dialog, 0, "unsaved changes Cancel"); sameValues(p, beforeCancel);
        require(p.presets.isModified(), "cancel preserves dirty sound");
        std::printf("UI: dirty navigation\n");
        nextButton->onClick(); pump();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "dirty navigation prompts again");
        closeAlertAndWait(dialog, 2, "unsaved changes Discard"); require(! p.presets.isModified(), "discard then load works");
        std::printf("UI: protecting asynchronous dialogs from DAW changes\n");
        set(p, "output", -14.6f);
        nextButton->onClick(); pump();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "dirty dialog opens for stale action test");
        p.setCurrentProgram(5); set(p, "output", -12.4f);
        const auto hostSound = p.presets.currentSound();
        closeAlertAndWait(dialog, 2, "stale Discard confirmation");
        require(p.presets.isCurrentSound(hostSound), "stale Discard cannot replace new DAW sound");
        dialog = waitForAlert("Preset library");
        closeAlertAndWait(dialog, 0, "stale Discard warning");
        const auto usersBeforeStaleSave = p.presets.userPresets().size();
        saveAsButton->onClick();
        dialog = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
        require(dialog != nullptr, "Save As opens for stale action test");
        dialog->getTextEditor("name")->setText("Should not save");
        set(p, "output", -11.1f);
        const auto automatedSound = p.presets.currentSound();
        closeAlertAndWait(dialog, 1, "stale Save As confirmation");
        require(p.presets.userPresets().size() == usersBeforeStaleSave && p.presets.isCurrentSound(automatedSound), "stale Save As preserves library and automated sound");
        dialog = waitForAlert("Preset library");
        closeAlertAndWait(dialog, 0, "stale Save As warning");
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
        dismissCalloutAndWait(callout);
        std::printf("PASS: editor sizes, Save As/Save UI, unsaved Cancel/Discard, browser and search.\n");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s\n", error.what()); return 1; }
}
