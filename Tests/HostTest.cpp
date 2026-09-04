#include <JuceHeader.h>
#include <cstdio>
#include <limits>

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
    stage("unloading module (descriptions, format manager)");
    descriptions.clear();
    stage("done");
    return 0;
}
