#include <JuceHeader.h>
#include <cstdio>
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

// Largest sample-to-sample step after a legato slide. A clean 65-98 Hz sine moves at most a few
// hundredths per sample; a phase reset mid-cycle produces a step of several tenths.
float renderLegatoStep()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f); setParameter(processor, "glide", 0.1f);
    setParameter(processor, "oneshot", 0.0f);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 8192); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 43, (juce::uint8) 110), 4096); // 36 still held
    processor.processBlock(audio, midi);
    float maxStep = 0.0f;
    for (int i = 4000; i < 8192; ++i)
        maxStep = std::max(maxStep, std::abs(audio.getSample(0, i) - audio.getSample(0, i - 1)));
    return maxStep;
}

// Envelope level 0.5 s into a note; must not depend on the sample rate.
float renderLevelAtHalfSecond(double sampleRate)
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 0.6f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f);
    processor.prepareToPlay(sampleRate, 512);
    const auto total = (int) (sampleRate * 0.6);
    juce::AudioBuffer<float> audio(2, total); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    processor.processBlock(audio, midi);
    const auto start = (int) (sampleRate * 0.5), window = (int) (sampleRate * 0.03);
    return audio.getMagnitude(0, start, window);
}

// Zero crossings over 0.5 s when Tune is changed while the note is held: the pitch must follow live.
int renderCrossingsWithTuneChangedMidNote(float tuneAfterHalf)
{
    SubLab808Processor processor;
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "decay", 4.0f);
    setParameter(processor, "tune", 0.0f);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 24000); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 110), 0);
    processor.processBlock(audio, midi);                   // first 0.5 s at tune 0
    setParameter(processor, "tune", tuneAfterHalf);
    midi.clear();
    processor.processBlock(audio, midi);                   // next 0.5 s, note still held
    int crossings = 0;
    for (int i = 1; i < 24000; ++i)
        if ((audio.getSample(0, i - 1) < 0.0f) != (audio.getSample(0, i) < 0.0f)) ++crossings;
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

    if (! (renderLegatoStep() < 0.05f)) return 13;
    // Tune must act live: +12 st doubles the zero-crossing rate of the held note.
    {
        const auto untuned = renderCrossingsWithTuneChangedMidNote(0.0f), tunedUp = renderCrossingsWithTuneChangedMidNote(12.0f);
        if (untuned < 50 || tunedUp < untuned * 19 / 10 || tunedUp > untuned * 21 / 10)
        {
            std::fprintf(stderr, "tune test: crossings untuned=%d tunedUp=%d\n", untuned, tunedUp);
            return 15;
        }
    }
    // Every factory preset value must lie inside its parameter range (guards the preset table).
    {
        SubLab808Processor table;
        for (int program = 0; program < table.getNumPrograms(); ++program)
            for (const auto* id : { "decay", "release", "punch", "pitchdecay", "glide", "tune", "body", "click", "drive", "tone", "velocity", "output" })
            {
                auto* parameter = dynamic_cast<juce::RangedAudioParameter*> (table.parameters.getParameter(id));
                const auto value = table.getFactoryPresetValue(program, id);
                if (parameter == nullptr || value < parameter->getNormalisableRange().start || value > parameter->getNormalisableRange().end) return 16;
            }
    }
    // Holding more notes than the tracking list can store must neither crash nor leave a stuck voice.
    {
        SubLab808Processor many;
        setParameter(many, "oneshot", 0.0f); setParameter(many, "release", 0.01f); setParameter(many, "decay", 4.0f);
        many.prepareToPlay(48000.0, 512);
        juce::AudioBuffer<float> manyAudio(2, 48000); juce::MidiBuffer manyMidi;
        for (int n = 0; n < 40; ++n) manyMidi.addEvent(juce::MidiMessage::noteOn(1, 24 + n, (juce::uint8) 100), n * 10);
        for (int n = 0; n < 40; ++n) manyMidi.addEvent(juce::MidiMessage::noteOff(1, 24 + n), 2000 + n * 10);
        many.processBlock(manyAudio, manyMidi);
        if (manyAudio.getMagnitude(0, 40000, 8000) > 0.001f) return 17;
    }
    {
        const auto at48 = renderLevelAtHalfSecond(48000.0), at96 = renderLevelAtHalfSecond(96000.0);
        if (at48 < 0.01f || std::abs(at48 - at96) > 0.05f * at48) return 14;
    }

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
    juce::MemoryOutputStream previewData;
    juce::PNGImageFormat png;
    if (! png.writeImageToStream(preview, previewData) || previewData.getDataSize() == 0) return 11;
    return 0;
}
