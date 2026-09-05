#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "FactoryBank.h"
#include <cmath>
#include <limits>

namespace
{
bool parseFiniteStateValue(const juce::var& encoded, double& result)
{
    if (encoded.isDouble() || encoded.isInt() || encoded.isInt64())
        result = static_cast<double>(encoded);
    else if (encoded.isString())
    {
        const auto text = encoded.toString().trim();
        if (text.isEmpty()) return false;
        auto cursor = text.getCharPointer();
        const auto parsed = juce::CharacterFunctions::readDoubleValue(cursor);
        if (! cursor.isEmpty() || ! std::isfinite(parsed)) return false;
        result = parsed;
    }
    else return false;
    return std::isfinite(result) && std::isfinite(static_cast<float>(result));
}
}

SubLab808Processor::SubLab808Processor(juce::File presetStorage)
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PARAMETERS", makeLayout()),
      presets(*this, parameters, "SubLab808", factoryBank(), std::move(presetStorage))
{
    clearHeldKeys();
    for (size_t i = 0; i < parameterIds.size(); ++i)
        rangedParameters[i] = parameters.getParameter(parameterIds[i]);
    ControlOperation initialProgram;
    initialProgram.programIndex = 0;
    initialProgram.notifyHost = false;
    submitControlOperation(std::move(initialProgram));
    for (const auto& [id, value] : factoryBank().front().values) { juce::ignoreUnused(value); parameters.addParameterListener(id, this); }
}

SubLab808Processor::~SubLab808Processor()
{
    cancelPendingUpdate();
    for (const auto& [id, value] : factoryBank().front().values) { juce::ignoreUnused(value); parameters.removeParameterListener(id, this); }
}

int SubLab808Processor::getNumPrograms() { return (int) factoryBank().size(); }

float SubLab808Processor::getFactoryPresetValue(int index, const juce::String& parameterId) const
{
    if (! juce::isPositiveAndBelow(index, (int) factoryBank().size())) return 0.0f;
    const auto& values = factoryBank()[static_cast<size_t>(index)].values;
    const auto it = values.find(parameterId);
    return it == values.end() ? 0.0f : it->second;
}

const juce::String SubLab808Processor::getProgramName(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return {};
    return factoryBank()[(size_t) index].name;
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
    const auto restoring = operation.kind == ControlOperation::Kind::state;
    if (! restoring && notificationOwner.load(std::memory_order_acquire) == juce::Thread::getCurrentThreadId())
    {
        // Only program echoes are suppressed during restore. A real nested STATE
        // always commits synchronously, even while an older notification is held.
        if (notifyingRestore.load(std::memory_order_acquire)) return;
        const std::lock_guard lock(controlMutex);
        operation.queuedProgram = true;
        operation.restoreEpoch = restoreEpoch;
        pendingProgramOperations.push_back(std::move(operation));
        return;
    }
    commitControlOperation(std::move(operation));
    drainStateNotifications();
    applyRestoredEditorSize();
    if (restoring) triggerAsyncUpdate(); // also deliver worker-thread size restores on the message thread
}

void SubLab808Processor::commitControlOperation(ControlOperation operation)
{
    const auto restoring = operation.kind == ControlOperation::Kind::state;
    std::array<float, parameterIds.size()> updates {};
    if (restoring)
    {
        operation.programIndex = juce::jlimit(0, getNumPrograms() - 1,
                                               (int) operation.state.getProperty("factoryProgram", 0));
        // Pinned APVTS creates absent PARAM children with defaults, and applies
        // the same default to a present child without "value". Last duplicate wins.
        for (size_t i = 0; i < parameterIds.size(); ++i) updates[i] = rangedParameters[i]->getDefaultValue();
        for (const auto& child : operation.state)
            for (size_t i = 0; i < parameterIds.size(); ++i)
                if (child["id"].toString() == parameterIds[i])
                {
                    const auto* parameter = rangedParameters[i];
                    updates[i] = child.hasProperty("value")
                        ? parameter->convertTo0to1(static_cast<float>(child["value"]))
                        : parameter->getDefaultValue();
                    break;
                }
    }
    else
    {
        const auto& values = factoryBank()[static_cast<size_t>(operation.programIndex)].values;
        for (size_t i = 0; i < parameterIds.size(); ++i)
            updates[i] = rangedParameters[i]->convertTo0to1(values.at(parameterIds[i]));
    }
    const std::lock_guard lock(controlMutex);
    if (operation.queuedProgram && operation.restoreEpoch != restoreEpoch) return;
    if (! restoring && currentProgram.load() == operation.programIndex && parametersMatchProgram(operation.programIndex))
    {
        presetModified.store(false);
        presets.clearSelection();
        return; // Coalesce a host echo; distinct queued A -> B -> A requests still commit in order.
    }
    parameterTransactionSequence.fetch_add(1, std::memory_order_seq_cst);
    const juce::ScopeGuard finishWrite { [this] { parameterTransactionSequence.fetch_add(1, std::memory_order_seq_cst); } };
    // Standard AudioParameterFloat/Bool setters have no listener callbacks. Never
    // call APVTS replaceState/copyState or notifying setters while holding this gate.
    for (size_t i = 0; i < parameterIds.size(); ++i) rangedParameters[i]->setValue(updates[i]);
    currentProgram.store(operation.programIndex);
    if (restoring)
    {
        stateExtensions = std::move(operation.state);
        const auto width = juce::jlimit(820, 1100, (int) stateExtensions.getProperty("editorWidth", 860));
        const auto height = juce::jlimit(430, 680, (int) stateExtensions.getProperty("editorHeight", 520));
        editorSize.store(packEditorSize(width, height) | editorRestorePendingMask);
        presetModified.store((bool) stateExtensions.getProperty("presetModified", false)
                             || ! parametersMatchProgram(operation.programIndex));
        clickSequenceGeneration.fetch_add(1, std::memory_order_seq_cst);
        ++restoreEpoch;
        pendingProgramOperations.clear(); // only requests older than this restore; later callbacks may enqueue again
        presets.restoreSelection(stateExtensions);
    }
    else
    {
        presetModified.store(! parametersMatchProgram(operation.programIndex));
        presets.clearSelection(); // publish selection revision only after the complete factory commit
    }
    committedProgramNotification = ! restoring && operation.notifyHost;
    committedRestoreNotification = restoring;
    controlGeneration.fetch_add(1, std::memory_order_release);
}

void SubLab808Processor::drainStateNotifications()
{
    juce::Thread::ThreadID expected = nullptr;
    if (! notificationOwner.compare_exchange_strong(expected, juce::Thread::getCurrentThreadId(),
                                                    std::memory_order_acq_rel, std::memory_order_acquire))
        return; // This caller may own a JUCE listener lock. Never wait for the dispatcher.
    internalParameterChangeDepth.fetch_add(1);
    const juce::ScopeGuard finish { [this] {
        internalParameterChangeDepth.fetch_sub(1);
        {
            const std::lock_guard lock(controlMutex);
            if (! parametersMatchProgram(currentProgram.load())) presetModified.store(true);
        }
        notifyingRestore.store(false, std::memory_order_release);
        notificationOwner.store(nullptr, std::memory_order_release);
        bool more = false;
        {
            const std::lock_guard lock(controlMutex);
            more = controlGeneration.load() != notifiedGeneration.load() || ! pendingProgramOperations.empty();
        }
        if (more) triggerAsyncUpdate(); // no lost queue entries or stranded generation on budget exhaustion
    } };
    unsigned remaining = maxGenerationsPerDrain;
    while (remaining > 0)
    {
        uint64_t generation = 0;
        bool notifyProgram = false;
        std::optional<ControlOperation> pending;
        {
            const std::lock_guard lock(controlMutex);
            generation = controlGeneration.load(std::memory_order_acquire);
            if (generation == notifiedGeneration.load(std::memory_order_acquire))
            {
                if (pendingProgramOperations.empty()) return;
                pending = std::move(pendingProgramOperations.front());
                pendingProgramOperations.pop_front();
            }
            notifyProgram = committedProgramNotification;
            notifyingRestore.store(committedRestoreNotification, std::memory_order_release);
        }
        if (pending.has_value())
        {
#if JUCE_STANDALONE_APPLICATION
            if (auto callback = beforeQueuedProgramCommitForTesting) callback();
#endif
            commitControlOperation(std::move(*pending));
            // A no-op host echo must not spend a notification generation or loop forever.
            if (generation == controlGeneration.load(std::memory_order_acquire)) --remaining;
            continue;
        }
        --remaining;
        bool superseded = false;
        for (auto* parameter : rangedParameters)
        {
            if (generation != controlGeneration.load(std::memory_order_acquire)) { superseded = true; break; }
            // Notify CURRENT values only. Never write an old notification argument
            // back to a parameter after a nested or independent state commit.
            parameter->sendValueChangedMessageToListeners(parameter->getValue());
            if (generation != controlGeneration.load(std::memory_order_acquire)) { superseded = true; break; }
        }
        if (superseded) continue;
        if (notifyProgram)
        {
            updateHostDisplay(ChangeDetails().withProgramChanged(true));
            if (generation != controlGeneration.load(std::memory_order_acquire)) continue;
        }
        notifiedGeneration.store(generation, std::memory_order_release);
    }
}

void SubLab808Processor::handleAsyncUpdate()
{
    drainStateNotifications();
    applyRestoredEditorSize();
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
    return { (int) (uint32_t) ((packed & ~editorRestorePendingMask) >> 32), (int) (uint32_t) packed };
}

void SubLab808Processor::setEditorSize(int width, int height) noexcept
{
    const auto desired = packEditorSize(juce::jlimit(820, 1100, width), juce::jlimit(430, 680, height));
    auto current = editorSize.load();
    while ((current & editorRestorePendingMask) == 0 && ! editorSize.compare_exchange_weak(current, desired)) {}
}

void SubLab808Processor::applyRestoredEditorSize()
{
    auto expected = editorSize.load();
    if ((expected & editorRestorePendingMask) == 0) return;
    if (auto* manager = juce::MessageManager::getInstanceWithoutCreating(); manager != nullptr && manager->isThisTheMessageThread())
    {
        if (auto* editor = getActiveEditor())
            editor->setSize((int) (uint32_t) ((expected & ~editorRestorePendingMask) >> 32), (int) (uint32_t) expected);
        const auto acknowledged = expected & ~editorRestorePendingMask;
        editorSize.compare_exchange_strong(expected, acknowledged);
    }
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

float SubLab808Processor::readParameter(size_t index) const noexcept
{
    const auto* parameter = rangedParameters[index];
    const auto fallback = parameter->convertFrom0to1(parameter->getDefaultValue());
    const auto normalised = parameter->getValue();
    if (! std::isfinite(normalised) || normalised < 0.0f || normalised > 1.0f) return fallback;
    const auto raw = parameter->convertFrom0to1(normalised);
    const auto& range = parameter->getNormalisableRange();
    return std::isfinite(raw) && raw >= range.start && raw <= range.end ? raw : fallback;
}

SubLab808Processor::RuntimeParameters SubLab808Processor::readRuntimeParameters() const
{
    RuntimeParameters result;
    result.decay = readParameter(0);
    result.release = readParameter(1);
    result.punch = readParameter(2);
    result.pitchDecay = readParameter(3);
    result.glide = readParameter(4);
    result.tune = readParameter(5);
    result.body = readParameter(6);
    result.click = readParameter(7);
    result.drive = readParameter(8);
    result.tone = readParameter(9);
    result.velocity = readParameter(10);
    result.output = readParameter(11);
    result.oneShot = readParameter(12) >= 0.5f;
    result.clickSequenceGeneration = clickSequenceGeneration.load(std::memory_order_seq_cst);
    return result;
}

void SubLab808Processor::refreshRuntimeParameters() noexcept
{
    const auto before = parameterTransactionSequence.load(std::memory_order_seq_cst);
    if ((before & 1u) != 0) return;

    const auto candidate = readRuntimeParameters();
    const auto after = parameterTransactionSequence.load(std::memory_order_seq_cst);
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
    {
        const std::lock_guard lock(controlMutex);
        runtimeParameters = readRuntimeParameters();
    }
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
    juce::ValueTree extensions, selection("PARAMETERS");
    std::array<float, parameterIds.size()> values;
    int program = 0;
    bool modified = false;
    juce::Point<int> size;
    {
        const std::lock_guard lock(controlMutex);
        extensions = stateExtensions;
        for (size_t i = 0; i < parameterIds.size(); ++i) values[i] = readParameter(i);
        program = currentProgram.load();
        modified = presetModified.load() || ! parametersMatchProgram(program);
        size = getEditorSize();
        presets.appendSelection(selection);
    }
    // Copy and serialize only our immutable state, never the APVTS handle. A
    // parameter callback can capture the complete committed state without waiting
    // for the old notification owner or taking JUCE's parameter-listener locks.
    auto state = extensions.createCopy();
    for (int child = state.getNumChildren(); --child >= 0;)
        if (state.getChild(child).hasType("WkPresetSelection")) state.removeChild(child, nullptr);
    if (const auto selected = selection.getChildWithName("WkPresetSelection"); selected.isValid())
        state.addChild(selected.createCopy(), -1, nullptr);
    for (size_t i = 0; i < parameterIds.size(); ++i)
    {
        juce::ValueTree parameter;
        for (int child = state.getNumChildren(); --child >= 0;)
            if (state.getChild(child)["id"].toString() == parameterIds[i])
            {
                parameter = state.getChild(child);
                break;
            }
        if (! parameter.isValid())
        {
            parameter = juce::ValueTree("PARAM");
            parameter.setProperty("id", parameterIds[i], nullptr);
            state.addChild(parameter, -1, nullptr);
        }
        parameter.setProperty("value", values[i], nullptr);
    }
    state.setProperty("factoryProgram", program, nullptr);
    state.setProperty("presetModified", modified, nullptr);
    state.setProperty("editorWidth", size.x, nullptr);
    state.setProperty("editorHeight", size.y, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

bool SubLab808Processor::parametersMatchProgram(int index)
{
    if (! juce::isPositiveAndBelow(index, getNumPrograms())) return false;
    const auto& preset = factoryBank()[(size_t) index];
    for (const auto& [id, value] : preset.values) {
        const auto* parameter = parameters.getParameter(id);
        if (parameter == nullptr) return false;
        const auto normalised = parameter->getValue();
        const auto expected = parameter->convertTo0to1(value);
        if (! std::isfinite(normalised) || normalised < 0.0f || normalised > 1.0f
            || ! std::isfinite(expected) || std::abs(normalised - expected) > 0.00002f) return false;
    }
    return true;
}

void SubLab808Processor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size)) {
        auto state = juce::ValueTree::fromXml(*xml);
        // The schema is fixed. Do not inspect the live APVTS handle before the
        // control operation is serialized: another restore may be replacing it.
        if (! state.hasType("PARAMETERS")) return;

        // Reject the complete state before submitting any part of it. A hostile or
        // damaged project must not commit metadata/reset Click while an invalid
        // parameter is silently clamped (or poison DSP with NaN/Inf).
        for (const auto& child : state)
        {
            const auto id = child["id"].toString();
            for (size_t index = 0; index < parameterIds.size(); ++index)
                if (id == parameterIds[index])
                {
                    if (! child.hasType("PARAM")) return;
                    if (! child.hasProperty("value")) break; // pinned legacy behavior: missing value uses the default
                    double value = 0.0;
                    const auto& range = rangedParameters[index]->getNormalisableRange();
                    if (! parseFiniteStateValue(child["value"], value)
                        || value < static_cast<double>(range.start)
                        || value > static_cast<double>(range.end)) return;
                    break;
                }
        }

        ControlOperation operation;
        operation.kind = ControlOperation::Kind::state;
        operation.notifyHost = false;
        operation.state = state.createCopy();
        submitControlOperation(std::move(operation));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SubLab808Processor(); }
