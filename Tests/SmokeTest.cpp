#include <JuceHeader.h>
#include "PluginProcessor.h"

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    SubLab808Processor processor;
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 4096);
    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    processor.processBlock(audio, midi);
    const auto peak = audio.getMagnitude(0, audio.getNumSamples());
    if (! std::isfinite(peak) || peak < 0.001f || peak > 1.5f) return 1;
    for (int program = 0; program < processor.getNumPrograms(); ++program) {
        processor.setCurrentProgram(program);
        if (processor.getProgramName(program).isEmpty()) return 5;
        audio.clear(); midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
        processor.processBlock(audio, midi);
        const auto programPeak = audio.getMagnitude(0, audio.getNumSamples());
        if (! std::isfinite(programPeak) || programPeak < 0.0001f || programPeak > 1.5f) return 6;
    }
    processor.setCurrentProgram(3);
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    if (state.isEmpty()) return 2;
    processor.setCurrentProgram(0);
    processor.setStateInformation(state.getData(), (int) state.getSize());
    if (processor.getCurrentProgram() != 3) return 7;
    processor.setCurrentProgram(0);
    if (auto* oneShot = processor.parameters.getParameter("oneshot")) oneShot->setValueNotifyingHost(0.0f);
    audio.clear(); midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 100), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, 12288), 128);
    midi.addEvent(juce::MidiMessage::noteOff(1, 36), 256);
    processor.processBlock(audio, midi);
    if (! std::isfinite(audio.getMagnitude(0, audio.getNumSamples()))) return 8;
    if (auto* oneShot = processor.parameters.getParameter("oneshot")) oneShot->setValueNotifyingHost(1.0f);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    editor->setSize(860, 520);
    auto preview = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    auto previewFile = juce::File::getCurrentWorkingDirectory().getChildFile("SubLab808-preview.png");
    previewFile.deleteFile();
    if (auto stream = previewFile.createOutputStream()) {
        juce::PNGImageFormat png;
        if (! png.writeImageToStream(preview, *stream)) return 3;
    } else return 4;
    return 0;
}
