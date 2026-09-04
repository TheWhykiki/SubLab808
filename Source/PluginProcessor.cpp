#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <limits>

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
    clearHeldKeys();
    ControlOperation initialProgram;
    initialProgram.programIndex = 0;
    initialProgram.notifyHost = false;
    submitControlOperation(std::move(initialProgram));
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
    return factoryPresets[(size_t) index].name;
}

void SubLab808Processor::setCurrentProgram(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return;

    ControlOperation operation;
    operation.programIndex = index;
    submitControlOperation(std::move(operation));
}

void SubLab808Processor::submitControlOperation(ControlOperation operation)
{
    const auto caller = juce::Thread::getCurrentThreadId();
    std::unique_lock lock(controlMutex);

    const auto enqueue = [this] (ControlOperation pending) {
        // Preserve finite FIFO cascades such as A -> B -> A, while placing a hard
        // ceiling on pathological listeners that request a new program forever.
        if (controlOperationBudget == 0) return;
        --controlOperationBudget;
        pendingControlOperations.push_back(std::move(pending));
    };

    // Calls made recursively from our own listener callbacks are deferred until
    // the outermost operation finishes. This prevents an old listener traversal
    // from resuming after a newer preset has already been published.
    if (controlOwner == caller) {
        if (stateRestoreActive.load()) return;
        enqueue(std::move(operation));
        return;
    }

    // AudioProcessor setters are synchronous: an independent control thread waits
    // for the current writer, then applies its operation before returning. Callers
    // must not create a cross-thread wait cycle by joining such a setter from one
    // of the processor's own synchronous callbacks.
    while (controlOwner != nullptr)
        controlCondition.wait(lock);

    controlOwner = caller;
    controlOperationBudget = maxControlOperationsPerDrain - 1;
    lock.unlock();

    for (;;) {
        performControlOperation(operation);

        lock.lock();
        if (pendingControlOperations.empty()) {
            controlOwner = nullptr;
            controlOperationBudget = 0;
            lock.unlock();
            controlCondition.notify_all();
            return;
        }
        operation = std::move(pendingControlOperations.front());
        pendingControlOperations.pop_front();
        lock.unlock();
    }
}

void SubLab808Processor::performControlOperation(const ControlOperation& operation)
{
    bool programChanged = false;
    {
        beginStateTransaction();
        const juce::ScopeGuard finishTransaction { [this] { endStateTransaction(); } };
        if (operation.kind == ControlOperation::Kind::program)
            programChanged = applyProgramNow(operation.programIndex);
        else
            applyStateNow(operation.state);
    }

    if (programChanged && operation.notifyHost) {
        updateHostDisplay(ChangeDetails().withProgramChanged(true));
    }
}

bool SubLab808Processor::applyProgramNow(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return false;

    if (currentProgram.load() == index && parametersMatchProgram(index)) {
        presetModified.store(false);
        return false;
    }

    struct ParameterUpdate { juce::RangedAudioParameter* parameter; float normalisedValue; };
    std::array<ParameterUpdate, presetFields.size() + 1> updates {};
    size_t updateCount = 0;

    internalParameterChangeDepth.fetch_add(1);
    const auto& preset = factoryPresets[(size_t) index];
    const auto stage = [&] (juce::RangedAudioParameter* parameter, float normalisedValue) {
        if (parameter == nullptr) return;
        parameter->setValue(normalisedValue);
        updates[updateCount++] = { parameter, parameter->getValue() };
    };
    for (const auto& field : presetFields)
        if (auto* parameter = parameters.getParameter(field.id))
            stage(parameter, parameter->convertTo0to1(preset.values.*field.member));
    stage(parameters.getParameter("oneshot"), preset.oneShot ? 1.0f : 0.0f);

    for (size_t i = 0; i < updateCount; ++i)
        updates[i].parameter->sendValueChangedMessageToListeners(updates[i].normalisedValue);

    currentProgram.store(index);
    // Do not blindly clear this flag: a host automation write may interleave with
    // the parameter notifications above. Check once while callbacks are suppressed,
    // then again after opening the callback gate to close both race windows.
    presetModified.store(! parametersMatchProgram(index));
    internalParameterChangeDepth.fetch_sub(1);
    if (! parametersMatchProgram(index)) presetModified.store(true);

    return true;
}

void SubLab808Processor::parameterChanged(const juce::String&, float)
{
    if (internalParameterChangeDepth.load() == 0) presetModified.store(true);
}

uint64_t SubLab808Processor::packEditorSize(int width, int height) noexcept
{
    return (uint64_t) (uint32_t) width << 32 | (uint32_t) height;
}

juce::Point<int> SubLab808Processor::getEditorSize() const noexcept
{
    const auto packed = editorSize.load();
    return { (int) (uint32_t) (packed >> 32), (int) (uint32_t) packed };
}

void SubLab808Processor::setEditorSize(int width, int height) noexcept
{
    editorSize.store(packEditorSize(juce::jlimit(820, 1100, width),
                                    juce::jlimit(430, 680, height)));
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

SubLab808Processor::RuntimeParameters SubLab808Processor::readRuntimeParameters() const
{
    RuntimeParameters result;
    result.decay = parameters.getRawParameterValue("decay")->load();
    result.release = parameters.getRawParameterValue("release")->load();
    result.punch = parameters.getRawParameterValue("punch")->load();
    result.pitchDecay = parameters.getRawParameterValue("pitchdecay")->load();
    result.glide = parameters.getRawParameterValue("glide")->load();
    result.tune = parameters.getRawParameterValue("tune")->load();
    result.body = parameters.getRawParameterValue("body")->load();
    result.click = parameters.getRawParameterValue("click")->load();
    result.drive = parameters.getRawParameterValue("drive")->load();
    result.tone = parameters.getRawParameterValue("tone")->load();
    result.velocity = parameters.getRawParameterValue("velocity")->load();
    result.output = parameters.getRawParameterValue("output")->load();
    result.oneShot = parameters.getRawParameterValue("oneshot")->load() >= 0.5f;
    result.clickSequenceGeneration = clickSequenceGeneration.load(std::memory_order_acquire);
    return result;
}

void SubLab808Processor::refreshRuntimeParameters() noexcept
{
    const auto before = parameterTransactionSequence.load(std::memory_order_acquire);
    if ((before & 1u) != 0) return;

    const auto candidate = readRuntimeParameters();
    const auto after = parameterTransactionSequence.load(std::memory_order_acquire);
    if (before == after) {
        // State restore only publishes a generation on the control thread. Adopt it
        // with the matching, committed parameters; noiseState remains audio-owned.
        if (candidate.clickSequenceGeneration != runtimeParameters.clickSequenceGeneration)
            noiseState = initialNoiseSeed;
        runtimeParameters = candidate;
    }
}

void SubLab808Processor::prepareToPlay(double sr, int)
{
    sampleRate = sr;
    resetSound();
    runtimeParameters = readRuntimeParameters();
    clickCoef = std::exp(-1.0f / (0.0006f * (float) sampleRate));
    ampCoef.reset(sampleRate, 0.02); pitchCoef.reset(sampleRate, 0.02);
    glideCoef.reset(sampleRate, 0.02); releaseCoef.reset(sampleRate, 0.02);
    drive.reset(sampleRate, 0.02); outputGain.reset(sampleRate, 0.02);
    filterCoef.reset(sampleRate, 0.02); body.reset(sampleRate, 0.02);
    tuneSemitones.reset(sampleRate, 0.005);
    for (auto& bend : channelBendSemitones) bend.reset(sampleRate, 0.005);
    const auto decay = runtimeParameters.decay;
    const auto pitchDecay = runtimeParameters.pitchDecay;
    const auto release = runtimeParameters.release;
    const auto glide = runtimeParameters.glide;
    ampCoef.setCurrentAndTargetValue(std::exp(-1.0f / (decay * (float) sampleRate)));
    pitchCoef.setCurrentAndTargetValue(std::exp(-1.0f / (pitchDecay * (float) sampleRate)));
    releaseCoef.setCurrentAndTargetValue(std::exp(-1.0f / (release * (float) sampleRate)));
    glideCoef.setCurrentAndTargetValue(glide <= 0.0001f ? 0.0f : (float) std::exp(-1.0 / (glide * sampleRate)));
    drive.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(runtimeParameters.drive));
    outputGain.setCurrentAndTargetValue(juce::Decibels::decibelsToGain(runtimeParameters.output));
    filterCoef.setCurrentAndTargetValue((float) std::exp(-juce::MathConstants<double>::twoPi * runtimeParameters.tone / sampleRate));
    body.setCurrentAndTargetValue(runtimeParameters.body * 0.01f);
    tuneSemitones.setCurrentAndTargetValue(std::round(runtimeParameters.tune));
}

void SubLab808Processor::resetSound()
{
    phase = 0.0; amp = 0.0f; pitchEnv = 0.0f; filterState = 0.0f; click = 0.0f;
    noiseState = initialNoiseSeed;
    active = false; gateReleased = false; currentKey = -1;
    for (auto& bend : channelBendSemitones) bend.setCurrentAndTargetValue(0.0f);
    clearHeldKeys();
}

int SubLab808Processor::keyForMidiMessage(int channel, int note) noexcept
{
    if (! juce::isPositiveAndBelow(channel, midiChannelCount + 1)
        || ! juce::isPositiveAndBelow(note, midiNoteCount)) return -1;
    return (channel - 1) * midiNoteCount + note;
}

void SubLab808Processor::removeHeldKeyFromOrder(int key)
{
    const auto previous = previousHeldKeys[(size_t) key];
    const auto next = nextHeldKeys[(size_t) key];
    if (previous < 0 && next < 0 && oldestHeldKey != key) return;

    if (previous >= 0) nextHeldKeys[(size_t) previous] = next;
    else oldestHeldKey = next;
    if (next >= 0) previousHeldKeys[(size_t) next] = previous;
    else newestHeldKey = previous;
    previousHeldKeys[(size_t) key] = -1;
    nextHeldKeys[(size_t) key] = -1;
    --numHeldKeys;
}

void SubLab808Processor::appendHeldKeyToOrder(int key)
{
    previousHeldKeys[(size_t) key] = newestHeldKey;
    nextHeldKeys[(size_t) key] = -1;
    if (newestHeldKey >= 0) nextHeldKeys[(size_t) newestHeldKey] = key;
    else oldestHeldKey = key;
    newestHeldKey = key;
    ++numHeldKeys;
}

void SubLab808Processor::clearHeldKeys()
{
    heldKeyCounts.fill(0);
    previousHeldKeys.fill(-1);
    nextHeldKeys.fill(-1);
    numHeldKeys = 0;
    oldestHeldKey = newestHeldKey = -1;
}

void SubLab808Processor::triggerNote(int channel, int note, float newVelocity)
{
    const auto key = keyForMidiMessage(channel, note);
    if (key < 0) return;

    // A new note played while another one is still held and Glide is active is a legato slide:
    // the pitch glides to the new note and the running envelope, phase and click continue.
    // Retriggering here would reset the phase to zero mid-cycle and click on every slide.
    const auto glideTime = runtimeParameters.glide;
    const auto currentNote = currentKey >= 0 ? currentKey % midiNoteCount : -1;
    const bool legato = active && numHeldKeys > 0 && note != currentNote && glideTime >= 0.001f;

    auto& holdCount = heldKeyCounts[(size_t) key];
    if (holdCount < std::numeric_limits<uint32_t>::max()) ++holdCount;
    removeHeldKeyFromOrder(key);
    appendHeldKeyToOrder(key);

    targetHz = juce::MidiMessage::getMidiNoteInHertz(note); // Tune is applied live in renderSample().
    if (! active || glideTime < 0.001f) currentHz = targetHz;
    if (legato) { currentKey = key; gateReleased = false; return; }
    auto velocityAmount = runtimeParameters.velocity * 0.01f;
    velocity = juce::jmap(velocityAmount, 1.0f, juce::jlimit(0.0f, 1.0f, newVelocity));
    amp = 1.0f;
    pitchEnv = runtimeParameters.punch;
    click = runtimeParameters.click * 0.01f;
    phase = 0.0;
    currentKey = key;
    gateReleased = false;
    active = true;
}

void SubLab808Processor::releaseNote(int channel, int note)
{
    const auto key = keyForMidiMessage(channel, note);
    if (key < 0) return;
    auto& holdCount = heldKeyCounts[(size_t) key];
    if (holdCount == 0) return;
    if (--holdCount > 0) return;

    removeHeldKeyFromOrder(key);
    if (runtimeParameters.oneShot) return;
    if (key != currentKey) return;
    if (numHeldKeys > 0) {
        currentKey = newestHeldKey;
        targetHz = juce::MidiMessage::getMidiNoteInHertz(currentKey % midiNoteCount);
        gateReleased = false;
    } else {
        gateReleased = true;
    }
}

void SubLab808Processor::allNotesOff(int channel)
{
    if (! juce::isPositiveAndBelow(channel, midiChannelCount + 1)) return;
    const auto channelIndex = channel - 1;
    const auto firstKey = channelIndex * midiNoteCount;
    const bool ownsCurrentVoice = currentKey >= firstKey && currentKey < firstKey + midiNoteCount;

    for (int note = 0; note < midiNoteCount; ++note) {
        const auto key = firstKey + note;
        heldKeyCounts[(size_t) key] = 0;
        removeHeldKeyFromOrder(key);
    }

    if (! ownsCurrentVoice || runtimeParameters.oneShot) return;
    if (numHeldKeys > 0) {
        currentKey = newestHeldKey;
        targetHz = juce::MidiMessage::getMidiNoteInHertz(currentKey % midiNoteCount);
        gateReleased = false;
    } else {
        gateReleased = true;
    }
}

void SubLab808Processor::allSoundOff(int channel)
{
    if (! juce::isPositiveAndBelow(channel, midiChannelCount + 1)) return;
    const auto channelIndex = channel - 1;
    const auto firstKey = channelIndex * midiNoteCount;
    const bool ownsCurrentVoice = currentKey >= firstKey && currentKey < firstKey + midiNoteCount;

    for (int note = 0; note < midiNoteCount; ++note) {
        const auto key = firstKey + note;
        heldKeyCounts[(size_t) key] = 0;
        removeHeldKeyFromOrder(key);
    }

    if (! ownsCurrentVoice) return;
    if (numHeldKeys > 0) {
        currentKey = newestHeldKey;
        targetHz = juce::MidiMessage::getMidiNoteInHertz(currentKey % midiNoteCount);
        gateReleased = false;
    } else {
        phase = 0.0; amp = 0.0f; pitchEnv = 0.0f; filterState = 0.0f; click = 0.0f;
        active = false; gateReleased = false; currentKey = -1;
    }
}

float SubLab808Processor::renderSample()
{
    const auto voiceChannel = currentKey >= 0 ? currentKey / midiNoteCount : -1;
    float bendSemitones = 0.0f;
    for (int channel = 0; channel < midiChannelCount; ++channel) {
        const auto bendNow = channelBendSemitones[(size_t) channel].getNextValue();
        if (channel == voiceChannel) bendSemitones = bendNow;
    }
    const auto tuneNow = tuneSemitones.getNextValue();

    // Idle voice: nothing left to render once the envelope, click and filter have died away.
    if (! active && std::abs(filterState) < 1.0e-6f && click < 1.0e-6f)
    {
        // Parameter automation must keep progressing during silence. Otherwise the next note
        // starts with stale drive, tone, body or output values and changes timbre for ~20 ms.
        ampCoef.skip(1); pitchCoef.skip(1); glideCoef.skip(1); releaseCoef.skip(1);
        drive.skip(1); outputGain.skip(1); filterCoef.skip(1); body.skip(1);
        filterState = 0.0f;
        return 0.0f;
    }
    const auto glideNow = glideCoef.getNextValue();
    currentHz = targetHz + (currentHz - targetHz) * glideNow;
    auto hz = currentHz * std::pow(2.0, (pitchEnv + bendSemitones + tuneNow) / 12.0f);
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
    // Both ramps follow audio time, including while their envelope phase is inactive.
    // Otherwise an earlier Release edit would only begin smoothing at NoteOff.
    const auto decayNow = ampCoef.getNextValue();
    const auto releaseNow = releaseCoef.getNextValue();
    amp *= gateReleased ? releaseNow : decayNow;
    pitchEnv *= pitchCoef.getNextValue();
    if (amp < amplitudeSilenceThreshold) {
        amp = 0.0f;
        active = false;
        currentKey = -1;
    }
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
    refreshRuntimeParameters();
    const auto decay = runtimeParameters.decay;
    const auto pitchDecay = runtimeParameters.pitchDecay;
    const auto release = runtimeParameters.release;
    const auto glide = runtimeParameters.glide;
    const auto driveDb = runtimeParameters.drive;
    const auto cutoff = runtimeParameters.tone;
    ampCoef.setTargetValue(std::exp(-1.0f / (decay * (float) sampleRate)));
    pitchCoef.setTargetValue(std::exp(-1.0f / (pitchDecay * (float) sampleRate)));
    releaseCoef.setTargetValue(std::exp(-1.0f / (release * (float) sampleRate)));
    glideCoef.setTargetValue(glide <= 0.0001f ? 0.0f : (float) std::exp(-1.0 / (glide * sampleRate)));
    filterCoef.setTargetValue((float) std::exp(-juce::MathConstants<double>::twoPi * cutoff / sampleRate));
    drive.setTargetValue(juce::Decibels::decibelsToGain(driveDb));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain(runtimeParameters.output));
    body.setTargetValue(runtimeParameters.body * 0.01f);
    tuneSemitones.setTargetValue(std::round(runtimeParameters.tune)); // live, unlike note-on only

    auto midiIterator = midi.begin();
    const auto midiEnd = midi.end();
    float peak = 0.0f;

    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        while (midiIterator != midiEnd && (*midiIterator).samplePosition <= i) {
            const auto metadata = *midiIterator;
            ++midiIterator;

            // This instrument only consumes MIDI channel messages, which are at most three
            // bytes long. Avoid materialising owning MidiMessage objects for SysEx payloads:
            // messages larger than MidiMessage's inline storage would otherwise allocate and
            // free heap memory inside processBlock().
            if (metadata.numBytes <= 0 || metadata.numBytes > 3
                || (metadata.data[0] & 0xf0u) == 0xf0u)
                continue;

            const auto message = metadata.getMessage();
            if (message.isNoteOn()) triggerNote(message.getChannel(), message.getNoteNumber(), message.getFloatVelocity());
            else if (message.isPitchWheel()) {
                const auto channelIndex = message.getChannel() - 1;
                if (juce::isPositiveAndBelow(channelIndex, midiChannelCount))
                    channelBendSemitones[(size_t) channelIndex].setTargetValue(
                        2.0f * (float) (message.getPitchWheelValue() - 8192) / 8192.0f);
            }
            else if (message.isNoteOff()) releaseNote(message.getChannel(), message.getNoteNumber());
            else if (message.isAllNotesOff()) allNotesOff(message.getChannel());
            else if (message.isAllSoundOff()) allSoundOff(message.getChannel());
            else if (message.isResetAllControllers()) {
                const auto channelIndex = message.getChannel() - 1;
                if (juce::isPositiveAndBelow(channelIndex, midiChannelCount))
                    channelBendSemitones[(size_t) channelIndex].setTargetValue(0.0f);
            }
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
    uint64_t observedGeneration = 0;
    {
        const juce::ScopedLock lock(stateSnapshotLock);
        if (stateTransactionActive) {
            dest = lastCommittedState;
            return;
        }
        observedGeneration = stateSnapshotGeneration;
    }

    auto snapshot = createStateSnapshot();
    {
        const juce::ScopedLock lock(stateSnapshotLock);
        if (stateTransactionActive || stateSnapshotGeneration != observedGeneration)
            dest = lastCommittedState;
        else {
            lastCommittedState = snapshot;
            dest = std::move(snapshot);
        }
    }
}

juce::MemoryBlock SubLab808Processor::createStateSnapshot()
{
    juce::MemoryBlock result;
    auto state = parameters.copyState();
    const auto size = getEditorSize();
    state.setProperty("factoryProgram", currentProgram.load(), nullptr);
    state.setProperty("presetModified", presetModified.load(), nullptr);
    state.setProperty("editorWidth", size.x, nullptr);
    state.setProperty("editorHeight", size.y, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, result);
    return result;
}

void SubLab808Processor::beginStateTransaction()
{
    auto baseline = createStateSnapshot();
    {
        const juce::ScopedLock lock(stateSnapshotLock);
        lastCommittedState = std::move(baseline);
        stateTransactionActive = true;
        ++stateSnapshotGeneration;
    }
    const auto previous = parameterTransactionSequence.fetch_add(1, std::memory_order_acq_rel);
    juce::ignoreUnused(previous);
    jassert((previous & 1u) == 0);
}

void SubLab808Processor::endStateTransaction()
{
    auto committed = createStateSnapshot();
    {
        const juce::ScopedLock lock(stateSnapshotLock);
        lastCommittedState = std::move(committed);
        stateTransactionActive = false;
        ++stateSnapshotGeneration;
    }
    const auto previous = parameterTransactionSequence.fetch_add(1, std::memory_order_release);
    juce::ignoreUnused(previous);
    jassert((previous & 1u) != 0);
}

bool SubLab808Processor::parametersMatchProgram(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return false;
    const auto& preset = factoryPresets[(size_t) index];
    for (const auto& field : presetFields) {
        const auto* value = parameters.getRawParameterValue(field.id);
        if (value == nullptr || std::abs(value->load() - preset.values.*field.member) > 0.0005f) return false;
    }
    const auto* oneShot = parameters.getRawParameterValue("oneshot");
    return oneShot != nullptr && (oneShot->load() >= 0.5f) == preset.oneShot;
}

bool SubLab808Processor::applyStateNow(const juce::ValueTree& state)
{
    if (! state.isValid() || state.getType() != parameters.state.getType()) return false;

    stateRestoreActive.store(true);
    const juce::ScopeGuard finishRestore { [this] { stateRestoreActive.store(false); } };
    const auto restoredProgram = juce::jlimit(0, getNumPrograms() - 1,
                                              (int) state.getProperty("factoryProgram", 0));
    const auto hasModifiedFlag = state.hasProperty("presetModified");
    const auto wasModified = (bool) state.getProperty("presetModified", false);
    const auto restoredWidth = (int) state.getProperty("editorWidth", 860);
    const auto restoredHeight = (int) state.getProperty("editorHeight", 520);

    internalParameterChangeDepth.fetch_add(1);
    parameters.replaceState(state);
    currentProgram.store(restoredProgram);
    setEditorSize(restoredWidth, restoredHeight);
    const auto matchesFactoryProgram = parametersMatchProgram(restoredProgram);
    presetModified.store((hasModifiedFlag && wasModified) || ! matchesFactoryProgram);
    internalParameterChangeDepth.fetch_sub(1);
    if (! parametersMatchProgram(restoredProgram)) presetModified.store(true);
    clickSequenceGeneration.fetch_add(1, std::memory_order_release);

    if (auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
        messageManager != nullptr && messageManager->isThisTheMessageThread())
        if (auto* editor = getActiveEditor()) {
            const auto restoredSize = getEditorSize();
            editor->setSize(restoredSize.x, restoredSize.y);
        }
    return true;
}

void SubLab808Processor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size)) {
        auto state = juce::ValueTree::fromXml(*xml);
        if (! state.isValid() || state.getType() != parameters.state.getType()) return;

        ControlOperation operation;
        operation.kind = ControlOperation::Kind::state;
        operation.notifyHost = false;
        operation.state = state.createCopy();
        submitControlOperation(std::move(operation));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SubLab808Processor(); }
