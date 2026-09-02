#include <JuceHeader.h>
#include <csignal>
#include <cstdio>

// Every stage reports to stderr so a CI failure identifies where the host test died. An abort
// handler makes SIGABRT (e.g. an uncaught exception or dyld/codesign refusal) visible as well.
namespace {
const char* currentStage = "startup";
void stage(const char* name) { currentStage = name; std::fprintf(stderr, "[host-test] %s\n", name); std::fflush(stderr); }
void onAbort(int) { std::fprintf(stderr, "[host-test] aborted during: %s\n", currentStage); std::fflush(stderr); std::_Exit(99); }
}

int runHostTest(int argc, char** argv);

int main(int argc, char** argv)
{
    std::signal(SIGABRT, onAbort);
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
    if (! instance->acceptsMidi() || instance->getName() != "SubLab808") return 4;

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
        peak = juce::jmax(peak, audio.getMagnitude(0, audio.getNumSamples()));
    }
    if (! std::isfinite(peak) || peak <= 0.0001f || peak > 1.001f) { std::fprintf(stderr, "[host-test] bad peak %f\n", (double) peak); return 5; }

    // Tear down in explicit steps so an abort during shutdown names the responsible stage.
    stage("releasing plugin instance");
    instance->releaseResources();
    instance.reset();
    stage("unloading module (descriptions, format manager)");
    descriptions.clear();
    stage("done");
    return 0;
}
