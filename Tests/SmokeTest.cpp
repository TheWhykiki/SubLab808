#include <JuceHeader.h>
#include "PluginProcessor.h"

namespace {
void setParameter(SubLab808Processor& processor, const char* id, float value)
{
    if (auto* parameter = processor.parameters.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

float renderGateTail(bool oneShot)
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "release", 0.01f);
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "drive", 0.0f);
    setParameter(processor, "oneshot", oneShot ? 1.0f : 0.0f);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 8192); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 48), 512);
    processor.processBlock(audio, midi);
    return audio.getMagnitude(0, 7168, 1024);
}

int countPositiveCrossings(const juce::AudioBuffer<float>& audio, int start, int length)
{
    int crossings = 0; auto previous = audio.getSample(0, start);
    for (int i = start + 1; i < start + length; ++i) {
        const auto current = audio.getSample(0, i);
        if (previous <= 0.0f && current > 0.0f) ++crossings;
        previous = current;
    }
    return crossings;
}

int renderPitchCrossings(int wheelValue)
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 24000); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(1, wheelValue), 0);
    processor.processBlock(audio, midi);
    return countPositiveCrossings(audio, 2048, 18000);
}
}

int main()
{
    juce::ScopedJuceInitialiser_GUI initialiseJuce;
    SubLab808Processor processor; processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    processor.processBlock(audio, midi);
    const auto peak = audio.getMagnitude(0, audio.getNumSamples());
    if (! std::isfinite(peak) || peak < 0.001f || peak > 1.001f) return 1;

    for (int program = 0; program < processor.getNumPrograms(); ++program) {
        processor.setCurrentProgram(program);
        if (processor.getProgramName(program).isEmpty()) return 2;
        audio.clear(); midi.clear();
        midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
        processor.processBlock(audio, midi);
        const auto programPeak = audio.getMagnitude(0, audio.getNumSamples());
        if (! std::isfinite(programPeak) || programPeak < 0.0001f || programPeak > 1.001f) return 3;
    }

    processor.setCurrentProgram(3); setParameter(processor, "drive", 13.7f);
    juce::MemoryBlock state; processor.getStateInformation(state);
    if (state.isEmpty()) return 4;
    processor.setCurrentProgram(0);
    processor.setStateInformation(state.getData(), (int) state.getSize());
    if (processor.getCurrentProgram() != 3 || ! processor.isPresetModified()) return 5;
    if (std::abs(processor.parameters.getRawParameterValue("drive")->load() - 13.7f) > 0.11f) return 6;

    processor.prepareToPlay(48000.0, 512); audio.clear(); midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 127), 0);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 128);
    processor.processBlock(audio, midi);
    if (audio.getMagnitude(0, 128, audio.getNumSamples() - 128) > 0.000001f) return 7;

    if (! (renderGateTail(false) < renderGateTail(true) * 0.1f)) return 8;
    if (! (renderPitchCrossings(16383) > renderPitchCrossings(8192) + 2)) return 9;

    SubLab808Processor hotProcessor;
    setParameter(hotProcessor, "output", 6.0f); setParameter(hotProcessor, "drive", 24.0f);
    setParameter(hotProcessor, "body", 100.0f); setParameter(hotProcessor, "click", 100.0f);
    hotProcessor.prepareToPlay(48000.0, 512); audio.clear(); midi.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 127), 0);
    hotProcessor.processBlock(audio, midi);
    if (audio.getMagnitude(0, audio.getNumSamples()) > 1.001f) return 10;

    processor.setCurrentProgram(0);
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    editor->setSize(820, 520);
    auto preview = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    auto previewFile = juce::File::getCurrentWorkingDirectory().getChildFile("SubLab808-preview.png");
    previewFile.deleteFile();
    if (auto stream = previewFile.createOutputStream()) {
        juce::PNGImageFormat png;
        if (! png.writeImageToStream(preview, *stream)) return 11;
    } else return 12;
    return 0;
}
