#include <JuceHeader.h>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    if (argc != 2) return 1;

    const juce::String pluginPath(argv[1]);
    juce::VST3PluginFormat scanner;
    juce::OwnedArray<juce::PluginDescription> descriptions;
    scanner.findAllTypesForFile(descriptions, pluginPath);
    if (descriptions.isEmpty()) return 2;

    juce::AudioPluginFormatManager manager;
    manager.addFormat(std::make_unique<juce::VST3PluginFormat>());
    juce::String error;
    auto instance = manager.createPluginInstance(*descriptions[0], 48000.0, 512, error);
    if (instance == nullptr) {
        juce::Logger::writeToLog(error);
        return 3;
    }
    if (! instance->acceptsMidi() || instance->getName() != "SubLab808") return 4;

    instance->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 2048);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    instance->processBlock(audio, midi);
    const auto peak = audio.getMagnitude(0, audio.getNumSamples());
    if (! std::isfinite(peak) || peak <= 0.0001f || peak > 1.001f) return 5;
    return 0;
}
