#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

// Every stage reports to stderr so a CI failure identifies where the host test died.
namespace {
void stage(const char* name) { std::fprintf(stderr, "[host-test] %s\n", name); std::fflush(stderr); }

struct AudioInspection
{
    bool finite = true;
    float peak = 0.0f;
    int invalidChannel = -1, invalidSample = -1;
};

AudioInspection inspectAudio(const juce::AudioBuffer<float>& audio)
{
    AudioInspection result;
    // Peak reductions can hide a NaN when comparisons select another finite value.
    // Check each channel/sample before reducing, and retain the exact failing location.
    for (int channel = 0; channel < audio.getNumChannels(); ++channel) {
        const auto* samples = audio.getReadPointer(channel);
        for (int sample = 0; sample < audio.getNumSamples(); ++sample) {
            if (! std::isfinite(samples[sample])) {
                result.finite = false;
                result.invalidChannel = channel;
                result.invalidSample = sample;
                return result;
            }
            result.peak = std::max(result.peak, std::abs(samples[sample]));
        }
    }
    return result;
}

bool audioValidatorSelfTest()
{
    juce::AudioBuffer<float> audio(2, 8);
    const auto reset = [&] {
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
                audio.setSample(channel, sample, (sample & 1) != 0 ? -0.25f : 0.5f);
    };
    reset();
    const auto finite = inspectAudio(audio);
    if (! finite.finite || finite.peak != 0.5f) return false;
    int acceptedInvalid = 0, mislocated = 0;
    for (const auto invalid : { std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity() })
        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (const auto sample : { 0, 3, 7 }) {
                reset();
                audio.setSample(channel, sample, invalid);
                const auto inspection = inspectAudio(audio);
                if (inspection.finite) ++acceptedInvalid;
                else if (inspection.invalidChannel != channel || inspection.invalidSample != sample) ++mislocated;
            }
    std::fprintf(stderr, "[host-test] sample validator: %d/18 invalid cases accepted, %d mislocated\n",
                 acceptedInvalid, mislocated);
    return acceptedInvalid == 0 && mislocated == 0;
}

bool inspectRenderedBlock(const juce::AudioBuffer<float>& audio, const char* context, int block, float& peak)
{
    const auto inspection = inspectAudio(audio);
    if (! inspection.finite) {
        std::fprintf(stderr, "[host-test] non-finite sample in %s: block=%d channel=%d sample=%d\n",
                     context, block, inspection.invalidChannel, inspection.invalidSample);
        return false;
    }
    peak = inspection.peak;
    return true;
}

bool setNormalisedParameter(juce::AudioProcessor& processor, const char* name, float value)
{
    for (auto* parameter : processor.getParameters())
        if (parameter->getName(128) == name) {
            parameter->setValueNotifyingHost(value);
            return true;
        }
    std::fprintf(stderr, "[host-test] parameter not found: %s\n", name);
    return false;
}

struct RecallSetting { const char* name; float requested; };
constexpr std::array<RecallSetting, 13> recallSettings {{
    { "Decay", 0.81f }, { "Release", 0.8f }, { "Pitch Punch", 0.125f },
    { "Pitch Decay", 0.72f }, { "Glide", 0.7f }, { "Tune", 0.625f },
    { "Body", 0.43f }, { "Click", 0.8f }, { "Drive", 0.35f },
    { "Tone", 0.9f }, { "Velocity", 0.55f }, { "Output", 0.5f }, { "One Shot", 0.0f }
}};

juce::AudioProcessorParameter* recallParameter(juce::AudioProcessor& processor, const char* name)
{
    for (auto* parameter : processor.getParameters())
        if (parameter->getName(128) == name) return parameter;
    std::fprintf(stderr, "[host-test] recall parameter missing: %s\n", name);
    return nullptr;
}

bool normalizedValue(float value)
{
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool readRecallParameters(juce::AudioProcessor& processor, std::vector<float>& values)
{
    values.clear();
    for (const auto& setting : recallSettings) {
        auto* parameter = recallParameter(processor, setting.name);
        if (parameter == nullptr || ! normalizedValue(parameter->getValue())) return false;
        values.push_back(parameter->getValue());
    }
    return true;
}

bool sameRecallParameters(const std::vector<float>& expected, const std::vector<float>& actual)
{
    if (expected.empty() || expected.size() != actual.size()) return false;
    for (size_t i = 0; i < expected.size(); ++i)
        // Allow only float normalization roundoff, below every legal parameter grid step.
        // The independently rendered audio below must still match sample-exactly.
        if (! normalizedValue(expected[i]) || ! normalizedValue(actual[i])
            || std::abs(expected[i] - actual[i]) >= 1.0e-6f) return false;
    return true;
}

bool sameRecallAudio(const juce::AudioBuffer<float>& expected, const juce::AudioBuffer<float>& actual,
                     bool reportMismatch = false)
{
    if (expected.getNumChannels() != actual.getNumChannels()
        || expected.getNumSamples() != actual.getNumSamples()
        || expected.getNumChannels() == 0 || expected.getNumSamples() == 0
        || ! inspectAudio(expected).finite || ! inspectAudio(actual).finite) return false;
    for (int channel = 0; channel < expected.getNumChannels(); ++channel)
        for (int sample = 0; sample < expected.getNumSamples(); ++sample)
            if (std::abs(expected.getSample(channel, sample) - actual.getSample(channel, sample)) > 0.0f) {
                if (reportMismatch)
                    std::fprintf(stderr, "[host-test] recall audio differs: channel=%d sample=%d before=%.9g after=%.9g\n",
                                 channel, sample, static_cast<double>(expected.getSample(channel, sample)),
                                 static_cast<double>(actual.getSample(channel, sample)));
                return false;
            }
    return true;
}

bool recallValidatorSelfTest()
{
    juce::AudioBuffer<float> good(2, 8), changed;
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < 8; ++sample) good.setSample(channel, sample, 0.25f);
    changed.makeCopyOf(good);
    const std::vector<float> parameters { 0.0f, 0.25f, 0.75f, 1.0f };
    if (! sameRecallAudio(good, changed) || ! sameRecallParameters(parameters, parameters)) return false;
    changed.setSample(1, 7, std::nextafter(0.25f, 1.0f));
    if (sameRecallAudio(good, changed)) return false; // Even one ULP must fail; no peak/RMS masking.
    auto wrong = parameters;
    wrong[1] += 2.0e-6f;
    if (sameRecallParameters(parameters, wrong)) return false;
    for (const auto invalid : { std::numeric_limits<float>::quiet_NaN(),
                               std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity() }) {
        changed.makeCopyOf(good); changed.setSample(1, 7, invalid);
        wrong = parameters; wrong[1] = invalid;
        if (sameRecallAudio(good, changed) || sameRecallAudio(changed, good) || sameRecallAudio(changed, changed)
            || sameRecallParameters(parameters, wrong) || sameRecallParameters(wrong, parameters)
            || sameRecallParameters(wrong, wrong)) return false;
    }
    for (const auto invalid : { -0.001f, 1.001f }) {
        wrong = parameters; wrong[1] = invalid;
        if (sameRecallParameters(wrong, wrong)) return false;
    }
    stage("recall validator: positive controls, wrong parameter, one-ULP audio change and invalid operands passed");
    return true;
}

bool renderRecallPhrase(juce::AudioPluginInstance& plugin, juce::AudioBuffer<float>& result)
{
    constexpr int blockSize = 512, totalSamples = 96000;
    // Both fresh instances receive this same prepare/MIDI history. prepareToPlay resets the
    // Click PRNG; restoring state must not be compared against a continued random sequence.
    plugin.prepareToPlay(48000.0, blockSize);
    result.setSize(2, totalSamples);
    juce::AudioBuffer<float> block(2, blockSize);
    juce::MidiBuffer midi;
    struct Event { int sample; juce::MidiMessage message; };
    const std::array<Event, 9> events {{
        { 16, juce::MidiMessage::noteOn(1, 36, (juce::uint8) 97) },
        { 8000, juce::MidiMessage::pitchWheel(1, 11000) },
        { 12000, juce::MidiMessage::noteOff(1, 36) },
        { 24000, juce::MidiMessage::pitchWheel(1, 8192) },
        { 24000, juce::MidiMessage::noteOn(1, 43, (juce::uint8) 111) },
        { 36000, juce::MidiMessage::noteOn(1, 48, (juce::uint8) 83) },
        { 42000, juce::MidiMessage::noteOff(1, 48) },
        { 48000, juce::MidiMessage::noteOff(1, 43) },
        { 72000, juce::MidiMessage::pitchWheel(1, 8192) }
    }};
    std::array<double, 2> energy {};
    for (int offset = 0; offset < totalSamples; offset += blockSize) {
        const auto count = juce::jmin(blockSize, totalSamples - offset);
        block.setSize(2, count, false, false, true); block.clear(); midi.clear();
        for (const auto& event : events)
            if (event.sample >= offset && event.sample < offset + count)
                midi.addEvent(event.message, event.sample - offset);
        plugin.processBlock(block, midi);
        float peak = 0.0f;
        if (! inspectRenderedBlock(block, "recall phrase", offset / blockSize, peak) || peak > 1.001f) {
            plugin.releaseResources(); return false;
        }
        for (int channel = 0; channel < 2; ++channel) {
            result.copyFrom(channel, offset, block, channel, 0, count);
            for (int sample = 0; sample < count; ++sample) {
                const auto value = static_cast<double>(block.getSample(channel, sample));
                energy[static_cast<size_t>(channel)] += value * value;
            }
        }
    }
    plugin.releaseResources();
    return energy[0] / totalSamples > 1.0e-6 && energy[1] / totalSamples > 1.0e-6;
}

bool bundleStateRecall(juce::AudioPluginFormatManager& manager, const juce::PluginDescription& description)
{
    juce::String error;
    stage("recall: creating reference VST3 and configuring non-default parameters");
    auto reference = manager.createPluginInstance(description, 48000.0, 512, error);
    if (reference == nullptr || reference->getTotalNumOutputChannels() != 2) return false;
    for (const auto& setting : recallSettings) {
        auto* parameter = recallParameter(*reference, setting.name);
        if (parameter == nullptr) return false;
        // Use the plugin's legal grid, not an unsnapped VST3 controller-cache request.
        const auto text = parameter->getText(setting.requested, 128);
        const auto canonical = parameter->getValueForText(text);
        std::fprintf(stderr, "[host-test] recall fixture %s: initial=%.9g canonical=%.9g text='%s'\n",
                     setting.name, static_cast<double>(parameter->getValue()),
                     static_cast<double>(canonical), text.toRawUTF8());
        if (! normalizedValue(canonical) || ! normalizedValue(parameter->getValue())
            || std::abs(canonical - parameter->getValue()) < 1.0e-4f) return false;
        parameter->setValueNotifyingHost(canonical);
        if (! normalizedValue(parameter->getValue())
            || std::abs(parameter->getValue() - canonical) >= 1.0e-6f) return false;
    }
    std::vector<float> expected, actual;
    if (! readRecallParameters(*reference, expected)) return false;
    stage("recall: saving state and rendering the reference phrase");
    juce::MemoryBlock state;
    reference->getStateInformation(state);
    if (state.getSize() == 0 || state.getSize() > static_cast<size_t>(std::numeric_limits<int>::max())) return false;
    juce::AudioBuffer<float> before, after;
    if (! renderRecallPhrase(*reference, before)) return false;
    reference.reset();
    stage("recall: creating a fresh VST3 from the same bundle");
    auto restored = manager.createPluginInstance(description, 48000.0, 512, error);
    if (restored == nullptr || restored->getTotalNumOutputChannels() != 2
        || ! readRecallParameters(*restored, actual)) return false;
    // A skipped/no-op state restore must fail on this real freshly loaded default instance.
    if (sameRecallParameters(expected, actual)) return false;
    stage("recall negative control: fresh default VST3 correctly differs from saved settings");
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    if (! readRecallParameters(*restored, actual) || ! sameRecallParameters(expected, actual)) {
        for (size_t i = 0; i < actual.size(); ++i)
            std::fprintf(stderr, "[host-test] recall %s: saved=%.9g restored=%.9g\n",
                         recallSettings[i].name, static_cast<double>(expected[i]), static_cast<double>(actual[i]));
        return false;
    }
    stage("recall: rendering restored phrase for sample-exact comparison");
    if (! renderRecallPhrase(*restored, after) || ! sameRecallAudio(before, after, true)) return false;
    stage("bundle state recall: 13 non-default parameters including Click, 96000 stereo frames sample-exact");
    return true;
}
}

int runHostTest(int argc, char** argv);

int main(int argc, char** argv)
{
    int result = 0;
    {
        juce::ScopedJuceInitialiser_GUI initialiseJuce;
        result = runHostTest(argc, argv);
        stage(result == 0 ? "shutting down JUCE" : "shutting down JUCE after failure");
    }
    stage("exited cleanly");
    return result;
}

int runHostTest(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage: SubLab808HostTests <path-to-vst3>\n"); return 1; }
    stage("validating per-sample audio checker");
    if (! audioValidatorSelfTest()) return 9;
    if (! recallValidatorSelfTest()) return 10;

    const juce::String pluginPath(argv[1]);
    std::fprintf(stderr, "[host-test] bundle: %s (exists: %d)\n", pluginPath.toRawUTF8(), (int) juce::File(pluginPath).exists());
    stage("scanning bundle");
    juce::VST3PluginFormat scanner;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    scanner.findAllTypesForFile(descriptions, pluginPath);
    if (descriptions.isEmpty()) { std::fprintf(stderr, "[host-test] no plugin descriptions found\n"); return 2; }
    std::fprintf(stderr, "[host-test] found: %s\n", descriptions[0]->name.toRawUTF8());

    juce::AudioPluginFormatManager manager;
    manager.addFormat(std::make_unique<juce::VST3PluginFormat>());
    juce::String error;
    stage("instantiating plugin");
    auto instance = manager.createPluginInstance(*descriptions[0], 48000.0, 512, error);
    if (instance == nullptr) {
        std::fprintf(stderr, "[host-test] instantiation failed: %s\n", error.toRawUTF8());
        return 3;
    }
    if (! instance->acceptsMidi()) {
        std::fprintf(stderr, "[host-test] plugin does not accept MIDI\n");
        return 4;
    }
    if (instance->getName() != "SubLab808") {
        std::fprintf(stderr, "[host-test] unexpected plugin name: %s\n", instance->getName().toRawUTF8());
        return 4;
    }
    const auto tail = instance->getTailLengthSeconds();
    if (! std::isfinite(tail) || tail < 46.1 || tail > 47.1) {
        std::fprintf(stderr, "[host-test] invalid tail: %.3f s\n", tail);
        return 6;
    }

    stage("rendering audio");
    // A VST3 host promises never to exceed the block size passed to prepareToPlay(); the JUCE
    // wrapper sizes its scratch buffers to exactly that. Render in blocks of that size, as a real
    // host does - a single oversized block would overflow the wrapper's buffers (heap corruption
    // that showed up as intermittent silence or an abort on teardown in CI).
    constexpr int blockSize = 512;
    instance->prepareToPlay(48000.0, blockSize);
    juce::AudioBuffer<float> audio(2, blockSize);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    float peak = 0.0f;
    for (int block = 0; block < 4; ++block)
    {
        instance->processBlock(audio, midi);
        midi.clear();
        float blockPeak = 0.0f;
        if (! inspectRenderedBlock(audio, "initial render", block, blockPeak)) return 5;
        peak = juce::jmax(peak, blockPeak);
    }
    if (! std::isfinite(peak) || peak <= 0.0001f || peak > 1.001f) { std::fprintf(stderr, "[host-test] bad peak %f\n", (double) peak); return 5; }

    stage("validating loaded-plugin tail");
    instance->releaseResources();
    const bool tailParametersSet = setNormalisedParameter(*instance, "Decay", 1.0f)
        && setNormalisedParameter(*instance, "Pitch Punch", 1.0f)
        && setNormalisedParameter(*instance, "Click", 1.0f)
        && setNormalisedParameter(*instance, "Body", 1.0f)
        && setNormalisedParameter(*instance, "Drive", 1.0f)
        && setNormalisedParameter(*instance, "Tone", 0.0f)
        && setNormalisedParameter(*instance, "Velocity", 1.0f)
        && setNormalisedParameter(*instance, "Output", 1.0f)
        && setNormalisedParameter(*instance, "One Shot", 1.0f);
    if (! tailParametersSet) return 7;

    instance->prepareToPlay(48000.0, blockSize);
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 127), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 36), 1);
    const auto tailBlocks = (int) std::ceil(tail * 48000.0 / blockSize);
    const auto eightSecondBlock = (8 * 48000) / blockSize;
    float peakAtEightSeconds = 0.0f, finalTailPeak = 0.0f;
    for (int block = 0; block < tailBlocks; ++block)
    {
        audio.clear();
        instance->processBlock(audio, midi);
        midi.clear();
        float blockPeak = 0.0f;
        if (! inspectRenderedBlock(audio, "tail render", block, blockPeak)) return 8;
        if (block == eightSecondBlock) peakAtEightSeconds = blockPeak;
        if (block == tailBlocks - 1) finalTailPeak = blockPeak;
    }
    if (peakAtEightSeconds < 0.01f || finalTailPeak > 1.0e-5f) {
        std::fprintf(stderr, "[host-test] invalid rendered tail: 8s=%g final=%g\n",
                     (double) peakAtEightSeconds, (double) finalTailPeak);
        return 8;
    }

    // Tear down in explicit steps so an abort during shutdown names the responsible stage.
    stage("releasing plugin instance");
    instance->releaseResources();
    instance.reset();
    stage("validating fresh-instance VST3 state recall");
    if (! bundleStateRecall(manager, *descriptions[0])) { stage("bundle state recall failed"); return 11; }
    stage("unloading module (descriptions, format manager)");
    descriptions.clear();
    stage("done");
    return 0;
}
