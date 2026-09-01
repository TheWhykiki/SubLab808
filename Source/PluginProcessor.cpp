#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
struct FactoryPreset { const char* name; std::array<float, 12> values; };
constexpr std::array<FactoryPreset, 8> factoryPresets {{
    { "Deep Foundation", { 1.40f, 0.18f, 12.0f, 0.070f, 0.030f, 0.0f, 10.0f, 5.0f, 2.0f, 1800.0f, 72.0f, -3.0f } },
    { "Modern Knock",    { 0.72f, 0.09f, 24.0f, 0.035f, 0.015f, 0.0f, 25.0f, 22.0f, 7.0f, 5200.0f, 90.0f, -3.0f } },
    { "Long Slide",      { 2.80f, 0.32f, 10.0f, 0.090f, 0.180f, 0.0f, 16.0f, 4.0f, 4.0f, 2600.0f, 65.0f, -4.0f } },
    { "Dirty Trunk",     { 1.10f, 0.12f, 18.0f, 0.055f, 0.045f, 0.0f, 42.0f, 12.0f, 16.0f, 3200.0f, 85.0f, -5.0f } },
    { "Short Punch",     { 0.28f, 0.05f, 32.0f, 0.022f, 0.000f, 0.0f, 30.0f, 38.0f, 9.0f, 7800.0f, 100.0f, -2.0f } },
    { "Soft Pillow",     { 1.75f, 0.40f, 7.0f, 0.110f, 0.060f, 0.0f, 5.0f, 0.0f, 1.0f, 1250.0f, 55.0f, -2.5f } },
    { "Upper Bass",      { 0.90f, 0.10f, 15.0f, 0.045f, 0.025f, 12.0f, 36.0f, 18.0f, 8.0f, 6400.0f, 82.0f, -4.0f } },
    { "Sub Destroyer",   { 1.55f, 0.16f, 28.0f, 0.030f, 0.040f, -12.0f, 55.0f, 15.0f, 22.0f, 4100.0f, 95.0f, -7.0f } }
}};
constexpr std::array<const char*, 12> presetParameterIds {
    "decay", "release", "punch", "pitchdecay", "glide", "tune", "body", "click", "drive", "tone", "velocity", "output"
};
}

SubLab808Processor::SubLab808Processor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", makeLayout())
{
    setCurrentProgram(0);
}

int SubLab808Processor::getNumPrograms() { return (int) factoryPresets.size(); }

const juce::String SubLab808Processor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms()) ? factoryPresets[(size_t) index].name : juce::String();
}

void SubLab808Processor::setCurrentProgram(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return;
    const auto& preset = factoryPresets[(size_t) index];
    for (size_t i = 0; i < presetParameterIds.size(); ++i)
        if (auto* parameter = parameters.getParameter(presetParameterIds[i]))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(preset.values[i]));
    currentProgram.store(index);
}

juce::AudioProcessorValueTreeState::ParameterLayout SubLab808Processor::makeLayout()
{
    using P = juce::AudioParameterFloat;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<P>("decay", "Decay", juce::NormalisableRange<float>(0.08f, 4.0f, 0.001f, 0.35f), 0.8f));
    p.push_back(std::make_unique<P>("release", "Release", juce::NormalisableRange<float>(0.01f, 1.5f, 0.001f, 0.4f), 0.12f));
    p.push_back(std::make_unique<P>("punch", "Pitch Punch", juce::NormalisableRange<float>(0.0f, 48.0f, 1.0f), 18.0f));
    p.push_back(std::make_unique<P>("pitchdecay", "Pitch Decay", juce::NormalisableRange<float>(0.005f, 0.3f, 0.001f, 0.4f), 0.045f));
    p.push_back(std::make_unique<P>("glide", "Glide", juce::NormalisableRange<float>(0.0f, 0.5f, 0.001f, 0.4f), 0.03f));
    p.push_back(std::make_unique<P>("tune", "Tune", juce::NormalisableRange<float>(-12.0f, 12.0f, 1.0f), 0.0f));
    p.push_back(std::make_unique<P>("body", "Body", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 18.0f));
    p.push_back(std::make_unique<P>("click", "Click", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 12.0f));
    p.push_back(std::make_unique<P>("drive", "Drive", juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 5.0f));
    p.push_back(std::make_unique<P>("tone", "Tone", juce::NormalisableRange<float>(80.0f, 12000.0f, 1.0f, 0.35f), 5000.0f));
    p.push_back(std::make_unique<P>("velocity", "Velocity", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 80.0f));
    p.push_back(std::make_unique<P>("output", "Output", juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f), -3.0f));
    p.push_back(std::make_unique<juce::AudioParameterBool>("oneshot", "One Shot", true));
    return { p.begin(), p.end() };
}

void SubLab808Processor::prepareToPlay(double sr, int)
{
    sampleRate = sr; phase = 0.0; amp = pitchEnv = filterState = 0.0f; active = false;
}

void SubLab808Processor::triggerNote(int note, float newVelocity)
{
    auto tune = parameters.getRawParameterValue("tune")->load();
    targetHz = juce::MidiMessage::getMidiNoteInHertz(note + (int) tune);
    if (! active || parameters.getRawParameterValue("glide")->load() < 0.001f) currentHz = targetHz;
    auto velocityAmount = parameters.getRawParameterValue("velocity")->load() * 0.01f;
    velocity = juce::jmap(velocityAmount, 1.0f, juce::jlimit(0.0f, 1.0f, newVelocity));
    amp = 1.0f;
    pitchEnv = parameters.getRawParameterValue("punch")->load();
    click = parameters.getRawParameterValue("click")->load() * 0.01f;
    phase = 0.0;
    currentNote = note;
    gateReleased = false;
    active = true;
}

float SubLab808Processor::renderSample()
{
    currentHz = targetHz + (currentHz - targetHz) * glideCoef;
    auto hz = currentHz * std::pow(2.0, (pitchEnv + bendSemitones) / 12.0f);
    phase += hz / sampleRate;
    phase -= std::floor(phase);
    auto fundamental = std::sin((float) (juce::MathConstants<double>::twoPi * phase));
    auto harmonic = std::sin((float) (juce::MathConstants<double>::twoPi * phase * 2.0));
    noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
    auto noise = (float) ((noiseState & 0xffffu) / 32767.5 - 1.0);
    auto transient = click * noise;
    click *= 0.965f;
    auto sample = (fundamental + body * 0.22f * harmonic + transient) * amp * velocity;
    sample = std::tanh(sample * drive) / std::max(1.0f, std::tanh(drive));
    filterState = (1.0f - filterCoef) * sample + filterCoef * filterState;
    amp *= gateReleased ? releaseCoef : ampCoef;
    pitchEnv *= pitchCoef;
    if (amp < 0.00001f) active = false;
    return filterState * outputGain;
}

bool SubLab808Processor::isBusesLayoutSupported(const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        || l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SubLab808Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals guard;
    buffer.clear();
    auto decay = parameters.getRawParameterValue("decay")->load();
    auto pitchDecay = parameters.getRawParameterValue("pitchdecay")->load();
    auto release = parameters.getRawParameterValue("release")->load();
    auto glide = parameters.getRawParameterValue("glide")->load();
    auto driveDb = parameters.getRawParameterValue("drive")->load();
    auto cutoff = parameters.getRawParameterValue("tone")->load();
    ampCoef = std::exp(-1.0f / (decay * (float) sampleRate));
    pitchCoef = std::exp(-1.0f / (pitchDecay * (float) sampleRate));
    releaseCoef = std::exp(-1.0f / (release * (float) sampleRate));
    glideCoef = glide <= 0.0001f ? 0.0f : (float) std::exp(-1.0 / (glide * sampleRate));
    filterCoef = (float) std::exp(-juce::MathConstants<double>::twoPi * cutoff / sampleRate);
    drive = juce::Decibels::decibelsToGain(driveDb);
    outputGain = juce::Decibels::decibelsToGain(parameters.getRawParameterValue("output")->load());
    body = parameters.getRawParameterValue("body")->load() * 0.01f;

    auto midiIterator = midi.begin();
    const auto midiEnd = midi.end();
    float peak = 0.0f;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        while (midiIterator != midiEnd && (*midiIterator).samplePosition <= i) {
            const auto message = (*midiIterator).getMessage();
            if (message.isNoteOn()) triggerNote(message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isPitchWheel()) bendSemitones = 2.0f * (message.getPitchWheelValue() - 8192) / 8192.0f;
            else if (message.isNoteOff() && message.getNoteNumber() == currentNote
                     && parameters.getRawParameterValue("oneshot")->load() < 0.5f) gateReleased = true;
            else if (message.isAllNotesOff()) gateReleased = true;
            else if (message.isAllSoundOff()) { active = false; amp = 0.0f; }
            ++midiIterator;
        }
        float sample = renderSample();
        for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.setSample(c, i, sample);
        peak = std::max(peak, std::abs(sample));
    }
    outputMeter.store(std::max(peak, outputMeter.load() * 0.86f));
}

juce::AudioProcessorEditor* SubLab808Processor::createEditor()
{
    return new SubLab808Editor(*this);
}

void SubLab808Processor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty("factoryProgram", currentProgram.load(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void SubLab808Processor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size)) {
        auto state = juce::ValueTree::fromXml(*xml);
        currentProgram.store(juce::jlimit(0, getNumPrograms() - 1, (int) state.getProperty("factoryProgram", 0)));
        parameters.replaceState(state);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SubLab808Processor(); }
