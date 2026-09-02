#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace {
// Preset values are named, so a column cannot be silently swapped; each field maps to its
// parameter through presetFields below (also used to validate ranges in the smoke tests).
struct PresetValues
{
    float decay, release, punch, pitchDecay, glide, tune, body, click, drive, tone, velocity, output;
};
struct FactoryPreset { const char* name; PresetValues values; bool oneShot; };
constexpr std::array<FactoryPreset, 8> factoryPresets {{
    { "Deep Foundation", { .decay = 1.40f, .release = 0.18f, .punch = 12.0f, .pitchDecay = 0.070f, .glide = 0.030f, .tune = 0.0f, .body = 10.0f, .click = 5.0f, .drive = 2.0f, .tone = 1800.0f, .velocity = 72.0f, .output = -3.0f }, true },
    { "Modern Knock",    { .decay = 0.72f, .release = 0.09f, .punch = 24.0f, .pitchDecay = 0.035f, .glide = 0.015f, .tune = 0.0f, .body = 25.0f, .click = 22.0f, .drive = 7.0f, .tone = 5200.0f, .velocity = 90.0f, .output = -3.0f }, true },
    { "Long Slide",      { .decay = 2.80f, .release = 0.32f, .punch = 10.0f, .pitchDecay = 0.090f, .glide = 0.180f, .tune = 0.0f, .body = 16.0f, .click = 4.0f, .drive = 4.0f, .tone = 2600.0f, .velocity = 65.0f, .output = -4.0f }, false },
    { "Dirty Trunk",     { .decay = 1.10f, .release = 0.12f, .punch = 18.0f, .pitchDecay = 0.055f, .glide = 0.045f, .tune = 0.0f, .body = 42.0f, .click = 12.0f, .drive = 16.0f, .tone = 3200.0f, .velocity = 85.0f, .output = -5.0f }, true },
    { "Short Punch",     { .decay = 0.28f, .release = 0.05f, .punch = 32.0f, .pitchDecay = 0.022f, .glide = 0.000f, .tune = 0.0f, .body = 30.0f, .click = 38.0f, .drive = 9.0f, .tone = 7800.0f, .velocity = 100.0f, .output = -2.0f }, true },
    { "Soft Pillow",     { .decay = 1.75f, .release = 0.40f, .punch = 7.0f, .pitchDecay = 0.110f, .glide = 0.060f, .tune = 0.0f, .body = 5.0f, .click = 0.0f, .drive = 1.0f, .tone = 1250.0f, .velocity = 55.0f, .output = -2.5f }, false },
    { "Upper Bass",      { .decay = 0.90f, .release = 0.10f, .punch = 15.0f, .pitchDecay = 0.045f, .glide = 0.025f, .tune = 12.0f, .body = 36.0f, .click = 18.0f, .drive = 8.0f, .tone = 6400.0f, .velocity = 82.0f, .output = -4.0f }, true },
    { "Sub Destroyer",   { .decay = 1.55f, .release = 0.16f, .punch = 28.0f, .pitchDecay = 0.030f, .glide = 0.040f, .tune = -12.0f, .body = 55.0f, .click = 15.0f, .drive = 22.0f, .tone = 4100.0f, .velocity = 95.0f, .output = -7.0f }, true }
}};
struct PresetField { const char* id; float PresetValues::* member; };
constexpr std::array<PresetField, 12> presetFields {{
    { "decay", &PresetValues::decay }, { "release", &PresetValues::release }, { "punch", &PresetValues::punch },
    { "pitchdecay", &PresetValues::pitchDecay }, { "glide", &PresetValues::glide }, { "tune", &PresetValues::tune },
    { "body", &PresetValues::body }, { "click", &PresetValues::click }, { "drive", &PresetValues::drive },
    { "tone", &PresetValues::tone }, { "velocity", &PresetValues::velocity }, { "output", &PresetValues::output }
}};
}

SubLab808Processor::SubLab808Processor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", makeLayout())
{
    setCurrentProgram(0);
    for (const auto& field : presetFields) parameters.addParameterListener(field.id, this);
    parameters.addParameterListener("oneshot", this);
}

SubLab808Processor::~SubLab808Processor()
{
    for (const auto& field : presetFields) parameters.removeParameterListener(field.id, this);
    parameters.removeParameterListener("oneshot", this);
}

int SubLab808Processor::getNumPrograms() { return (int) factoryPresets.size(); }

float SubLab808Processor::getFactoryPresetValue(int index, const juce::String& parameterId) const
{
    if (! juce::isPositiveAndBelow(index, (int) factoryPresets.size())) return 0.0f;
    if (parameterId == "oneshot") return factoryPresets[(size_t) index].oneShot ? 1.0f : 0.0f;
    for (const auto& field : presetFields)
        if (parameterId == field.id) return factoryPresets[(size_t) index].values.*field.member;
    return 0.0f;
}

const juce::String SubLab808Processor::getProgramName(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return {};
    auto name = juce::String(factoryPresets[(size_t) index].name);
    if (index == currentProgram.load() && presetModified.load()) name += " (Modified)";
    return name;
}

void SubLab808Processor::setCurrentProgram(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return;
    applyingPreset.store(true);
    const auto& preset = factoryPresets[(size_t) index];
    for (const auto& field : presetFields)
        if (auto* parameter = parameters.getParameter(field.id))
            parameter->setValueNotifyingHost(parameter->convertTo0to1(preset.values.*field.member));
    if (auto* parameter = parameters.getParameter("oneshot"))
        parameter->setValueNotifyingHost(preset.oneShot ? 1.0f : 0.0f);
    currentProgram.store(index);
    presetModified.store(false);
    applyingPreset.store(false);
    updateHostDisplay(ChangeDetails().withProgramChanged(true));
}

void SubLab808Processor::parameterChanged(const juce::String&, float)
{
    if (! applyingPreset.load()) presetModified.store(true);
}

juce::AudioProcessorValueTreeState::ParameterLayout SubLab808Processor::makeLayout()
{
    using P = juce::AudioParameterFloat;
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<P>(juce::ParameterID { "decay", 1 }, "Decay", juce::NormalisableRange<float>(0.08f, 4.0f, 0.001f, 0.35f), 0.8f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "release", 1 }, "Release", juce::NormalisableRange<float>(0.01f, 1.5f, 0.001f, 0.4f), 0.12f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "punch", 1 }, "Pitch Punch", juce::NormalisableRange<float>(0.0f, 48.0f, 1.0f), 18.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "pitchdecay", 1 }, "Pitch Decay", juce::NormalisableRange<float>(0.005f, 0.3f, 0.001f, 0.4f), 0.045f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "glide", 1 }, "Glide", juce::NormalisableRange<float>(0.0f, 0.5f, 0.001f, 0.4f), 0.03f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "tune", 1 }, "Tune", juce::NormalisableRange<float>(-12.0f, 12.0f, 1.0f), 0.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "body", 1 }, "Body", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 18.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "click", 1 }, "Click", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 12.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "drive", 1 }, "Drive", juce::NormalisableRange<float>(0.0f, 24.0f, 0.1f), 5.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "tone", 1 }, "Tone", juce::NormalisableRange<float>(80.0f, 12000.0f, 1.0f, 0.35f), 5000.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "velocity", 1 }, "Velocity", juce::NormalisableRange<float>(0.0f, 100.0f, 1.0f), 80.0f));
    p.push_back(std::make_unique<P>(juce::ParameterID { "output", 1 }, "Output", juce::NormalisableRange<float>(-24.0f, 6.0f, 0.1f), -3.0f));
    p.push_back(std::make_unique<juce::AudioParameterBool>(juce::ParameterID { "oneshot", 1 }, "One Shot", true));
    return { p.begin(), p.end() };
}

void SubLab808Processor::prepareToPlay(double sr, int)
{
    sampleRate = sr;
    resetSound();
    clickCoef = std::exp(-1.0f / (0.0006f * (float) sampleRate));
    ampCoef.reset(sampleRate, 0.02); pitchCoef.reset(sampleRate, 0.02);
    glideCoef.reset(sampleRate, 0.02); releaseCoef.reset(sampleRate, 0.02);
    drive.reset(sampleRate, 0.02); outputGain.reset(sampleRate, 0.02);
    filterCoef.reset(sampleRate, 0.02); body.reset(sampleRate, 0.02);
    const auto decay = parameters.getRawParameterValue("decay")->load();
    const auto pitchDecay = parameters.getRawParameterValue("pitchdecay")->load();
    const auto release = parameters.getRawParameterValue("release")->load();
    const auto glide = parameters.getRawParameterValue("glide")->load();
    ampCoef.setCurrentAndTargetValue(std::exp(-1.0f / (decay * (float) sampleRate)));
    pitchCoef.setCurrentAndTargetValue(std::exp(-1.0f / (pitchDecay * (float) sampleRate)));
    releaseCoef.setCurrentAndTargetValue(std::exp(-1.0f / (release * (float) sampleRate)));
    glideCoef.setCurrentAndTargetValue(glide <= 0.0001f ? 0.0f : (float) std::exp(-1.0 / (glide * sampleRate)));
    drive.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(parameters.getRawParameterValue("drive")->load()));
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(parameters.getRawParameterValue("output")->load()));
    filterCoef.setCurrentAndTargetValue((float) std::exp(-juce::MathConstants<double>::twoPi * parameters.getRawParameterValue("tone")->load() / sampleRate));
    body.setCurrentAndTargetValue(parameters.getRawParameterValue("body")->load() * 0.01f);
    tuneSemitones = std::round(parameters.getRawParameterValue("tune")->load());
}

void SubLab808Processor::resetSound()
{
    phase = 0.0; amp = 0.0f; pitchEnv = 0.0f; filterState = 0.0f; click = 0.0f;
    bendSemitones = 0.0f; active = false; gateReleased = false; currentNote = -1; numHeldNotes = 0;
}

void SubLab808Processor::triggerNote(int note, float newVelocity)
{
    for (int i = numHeldNotes - 1; i >= 0; --i)
        if (heldNotes[(size_t) i] == note) {
            for (int j = i; j < numHeldNotes - 1; ++j) heldNotes[(size_t) j] = heldNotes[(size_t) (j + 1)];
            --numHeldNotes;
        }
    // A new note played while another one is still held and Glide is active is a legato slide:
    // the pitch glides to the new note and the running envelope, phase and click continue.
    // Retriggering here would reset the phase to zero mid-cycle and click on every slide.
    const auto glideTime = parameters.getRawParameterValue("glide")->load();
    const bool legato = active && numHeldNotes > 0 && glideTime >= 0.001f;
    if (numHeldNotes == (int) heldNotes.size()) // full: forget the oldest note instead of the new one
    {
        for (int j = 0; j < numHeldNotes - 1; ++j) heldNotes[(size_t) j] = heldNotes[(size_t) (j + 1)];
        --numHeldNotes;
    }
    heldNotes[(size_t) numHeldNotes++] = note;
    targetHz = juce::MidiMessage::getMidiNoteInHertz(note); // Tune is applied live in renderSample()
    if (! active || glideTime < 0.001f) currentHz = targetHz;
    if (legato) { currentNote = note; gateReleased = false; return; }
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

void SubLab808Processor::releaseNote(int note)
{
    for (int i = numHeldNotes - 1; i >= 0; --i)
        if (heldNotes[(size_t) i] == note) {
            for (int j = i; j < numHeldNotes - 1; ++j) heldNotes[(size_t) j] = heldNotes[(size_t) (j + 1)];
            --numHeldNotes;
            break;
        }
    if (note != currentNote) return;
    if (numHeldNotes > 0) {
        currentNote = heldNotes[(size_t) (numHeldNotes - 1)];
        targetHz = juce::MidiMessage::getMidiNoteInHertz(currentNote);
        gateReleased = false;
    } else {
        currentNote = -1;
        if (parameters.getRawParameterValue("oneshot")->load() < 0.5f) gateReleased = true;
    }
}

float SubLab808Processor::renderSample()
{
    // Idle voice: nothing left to render once the envelope, click and filter have died away.
    if (! active && std::abs(filterState) < 1.0e-6f && click < 1.0e-6f)
    {
        filterState = 0.0f;
        return 0.0f;
    }
    const auto glideNow = glideCoef.getNextValue();
    currentHz = targetHz + (currentHz - targetHz) * glideNow;
    auto hz = currentHz * std::pow(2.0, (pitchEnv + bendSemitones + tuneSemitones) / 12.0f);
    phase += hz / sampleRate;
    phase -= std::floor(phase);
    auto fundamental = std::sin((float) (juce::MathConstants<double>::twoPi * phase));
    auto harmonic = std::sin((float) (juce::MathConstants<double>::twoPi * phase * 2.0));
    noiseState ^= noiseState << 13; noiseState ^= noiseState >> 17; noiseState ^= noiseState << 5;
    auto noise = (float) ((noiseState & 0xffffu) / 32767.5 - 1.0);
    auto transient = click * noise;
    click *= clickCoef;
    const auto bodyNow = body.getNextValue();
    const auto driveNow = drive.getNextValue();
    const auto filterNow = filterCoef.getNextValue();
    auto sample = (fundamental + bodyNow * 0.22f * harmonic + transient) * amp * velocity;
    sample = std::tanh(sample * driveNow) / std::max(1.0f, std::tanh(driveNow));
    filterState = (1.0f - filterNow) * sample + filterNow * filterState;
    amp *= gateReleased ? releaseCoef.getNextValue() : ampCoef.getNextValue();
    pitchEnv *= pitchCoef.getNextValue();
    if (amp < 0.00001f) active = false;
    const auto postGain = filterState * outputGain.getNextValue();
    return std::tanh(postGain * 1.2f);
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
    ampCoef.setTargetValue(std::exp(-1.0f / (decay * (float) sampleRate)));
    pitchCoef.setTargetValue(std::exp(-1.0f / (pitchDecay * (float) sampleRate)));
    releaseCoef.setTargetValue(std::exp(-1.0f / (release * (float) sampleRate)));
    glideCoef.setTargetValue(glide <= 0.0001f ? 0.0f : (float) std::exp(-1.0 / (glide * sampleRate)));
    filterCoef.setTargetValue((float) std::exp(-juce::MathConstants<double>::twoPi * cutoff / sampleRate));
    drive.setTargetValue(juce::Decibels::decibelsToGain(driveDb));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(parameters.getRawParameterValue("output")->load()));
    body.setTargetValue(parameters.getRawParameterValue("body")->load() * 0.01f);
    tuneSemitones = std::round(parameters.getRawParameterValue("tune")->load()); // live, unlike note-on only

    auto midiIterator = midi.begin();
    const auto midiEnd = midi.end();
    float peak = 0.0f;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        while (midiIterator != midiEnd && (*midiIterator).samplePosition <= i) {
            const auto message = (*midiIterator).getMessage();
            if (message.isNoteOn()) triggerNote(message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isPitchWheel()) bendSemitones = 2.0f * (float) (message.getPitchWheelValue() - 8192) / 8192.0f;
            else if (message.isNoteOff()) releaseNote(message.getNoteNumber());
            else if (message.isAllNotesOff())
            {
                numHeldNotes = 0; currentNote = -1; bendSemitones = 0.0f;
                if (parameters.getRawParameterValue("oneshot")->load() < 0.5f) gateReleased = true; // one-shot keeps decaying
            }
            else if (message.isAllSoundOff()) resetSound();
            ++midiIterator;
        }
        float sample = renderSample();
        for (int c = 0; c < buffer.getNumChannels(); ++c) buffer.setSample(c, i, sample);
        peak = std::max(peak, std::abs(sample));
    }
    auto previousPeak = outputMeter.load();
    while (previousPeak < peak && ! outputMeter.compare_exchange_weak(previousPeak, peak)) {}
}

juce::AudioProcessorEditor* SubLab808Processor::createEditor()
{
    return new SubLab808Editor(*this);
}

void SubLab808Processor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty("factoryProgram", currentProgram.load(), nullptr);
    state.setProperty("presetModified", presetModified.load(), nullptr);
    state.setProperty("editorWidth", editorWidth.load(), nullptr);
    state.setProperty("editorHeight", editorHeight.load(), nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void SubLab808Processor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size)) {
        auto state = juce::ValueTree::fromXml(*xml);
        currentProgram.store(juce::jlimit(0, getNumPrograms() - 1, (int) state.getProperty("factoryProgram", 0)));
        const auto wasModified = (bool) state.getProperty("presetModified", false);
        editorWidth.store(juce::jlimit(820, 1100, (int) state.getProperty("editorWidth", 860)));
        editorHeight.store(juce::jlimit(430, 680, (int) state.getProperty("editorHeight", 520)));
        applyingPreset.store(true);
        parameters.replaceState(state);
        applyingPreset.store(false);
        presetModified.store(wasModified);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SubLab808Processor(); }
