#include <JuceHeader.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <new>
#include "PluginProcessor.h"
#include <thread>
#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SUBLAB808_ADDRESS_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define SUBLAB808_ADDRESS_SANITIZER 1
#endif
#ifndef SUBLAB808_ADDRESS_SANITIZER
#define SUBLAB808_ADDRESS_SANITIZER 0
#endif

#if defined(__APPLE__) && !SUBLAB808_ADDRESS_SANITIZER
namespace allocationProbe
{
thread_local bool enabled = false;
thread_local size_t allocations = 0;

static void begin() noexcept
{
    allocations = 0;
    enabled = true;
}

static size_t end() noexcept
{
    enabled = false;
    return allocations;
}
}

// JUCE's long MidiMessage storage uses std::malloc directly, not operator new. Delegate to the
// default zone so this test executable can count those calls without changing allocator semantics.
extern "C" void* malloc(std::size_t size)
{
    if (allocationProbe::enabled)
        ++allocationProbe::allocations;
    auto* memory = malloc_zone_malloc(malloc_default_zone(), size);
    if (memory == nullptr)
        errno = ENOMEM;
    return memory;
}

void* operator new(std::size_t size)
{
    if (allocationProbe::enabled)
        ++allocationProbe::allocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc {};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }
#endif

namespace {
void setParameter(SubLab808Processor& processor, const char* id, float value)
{
    if (auto* parameter = processor.parameters.getParameter(id))
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

bool matchesFactoryProgram(const SubLab808Processor& processor, int program)
{
    for (const auto* id : { "decay", "release", "punch", "pitchdecay", "glide", "tune",
                            "body", "click", "drive", "tone", "velocity", "output" })
        if (std::abs(processor.parameters.getRawParameterValue(id)->load()
                     - processor.getFactoryPresetValue(program, id)) > 0.0005f) return false;
    return (processor.parameters.getRawParameterValue("oneshot")->load() >= 0.5f)
        == (processor.getFactoryPresetValue(program, "oneshot") >= 0.5f);
}

juce::MemoryBlock withoutStateProperty(const juce::MemoryBlock& state, const char* property)
{
    juce::MemoryBlock result;
    if (auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int) state.getSize())) {
        xml->removeAttribute(property);
        juce::AudioProcessor::copyXmlToBinary(*xml, result);
    }
    return result;
}

juce::MemoryBlock withStateProperty(const juce::MemoryBlock& state, const char* property, const juce::var& value)
{
    juce::MemoryBlock result;
    if (auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int) state.getSize())) {
        xml->setAttribute(property, value.toString());
        juce::AudioProcessor::copyXmlToBinary(*xml, result);
    }
    return result;
}

juce::MemoryBlock legacyDirtyTrunkStateFixture()
{
    // Frozen pre-presetModified APVTS schema. Keeping this independent from the
    // current serializer catches accidental root, child or parameter-ID changes.
    static constexpr auto xmlText = R"xml(
<PARAMETERS factoryProgram="3" editorWidth="860" editorHeight="520">
  <PARAM id="decay" value="1.1"/>
  <PARAM id="release" value="0.12"/>
  <PARAM id="punch" value="18.0"/>
  <PARAM id="pitchdecay" value="0.055"/>
  <PARAM id="glide" value="0.045"/>
  <PARAM id="tune" value="0.0"/>
  <PARAM id="body" value="42.0"/>
  <PARAM id="click" value="12.0"/>
  <PARAM id="drive" value="16.0"/>
  <PARAM id="tone" value="3200.0"/>
  <PARAM id="velocity" value="85.0"/>
  <PARAM id="output" value="-5.0"/>
  <PARAM id="oneshot" value="1.0"/>
</PARAMETERS>)xml";

    juce::MemoryBlock result;
    if (auto xml = juce::XmlDocument::parse(juce::String::fromUTF8(xmlText)))
        juce::AudioProcessor::copyXmlToBinary(*xml, result);
    return result;
}

struct ReentrantPresetEdit final : juce::AudioProcessorParameter::Listener
{
    ReentrantPresetEdit(juce::RangedAudioParameter& parameterIn, float replacementValue)
        : parameter(parameterIn), replacement(parameter.convertTo0to1(replacementValue)) {}

    void parameterValueChanged(int, float) override
    {
        if (! armed) return;
        armed = false;
        parameter.setValueNotifyingHost(replacement);
    }

    void parameterGestureChanged(int, bool) override {}

    juce::RangedAudioParameter& parameter;
    float replacement;
    bool armed = true;
};

struct ReentrantProgramSelection final : juce::AudioProcessorParameter::Listener
{
    ReentrantProgramSelection(SubLab808Processor& processorIn, int requestedProgram)
        : processor(processorIn), program(requestedProgram) {}

    void parameterValueChanged(int, float) override
    {
        if (! armed) return;
        armed = false;
        processor.setCurrentProgram(program);
        observedProgramAfterCall = processor.getCurrentProgram();
        observedDecayAfterCall = processor.parameters.getRawParameterValue("decay")->load();
    }

    void parameterGestureChanged(int, bool) override {}

    SubLab808Processor& processor;
    int program;
    int observedProgramAfterCall = -1;
    float observedDecayAfterCall = -1.0f;
    bool armed = true;
};

struct EchoingProgramHost final : juce::AudioProcessorListener
{
    explicit EchoingProgramHost(SubLab808Processor& processorIn) : processor(processorIn) {}

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}

    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (! details.programChanged) return;
        ++programNotifications;
        processor.setCurrentProgram(processor.getCurrentProgram());
    }

    SubLab808Processor& processor;
    int programNotifications = 0;
};

struct CascadingProgramHost final : juce::AudioProcessorListener
{
    explicit CascadingProgramHost(SubLab808Processor& processorIn) : processor(processorIn) {}

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}

    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (! details.programChanged) return;
        programs.push_back(processor.getCurrentProgram());
        if (step == 0) {
            ++step;
            processor.setCurrentProgram(2);
        } else if (step == 1) {
            ++step;
            processor.setCurrentProgram(1);
        }
    }

    SubLab808Processor& processor;
    std::vector<int> programs;
    int step = 0;
};

struct CrossThreadStateHost final : juce::AudioProcessorListener
{
    explicit CrossThreadStateHost(SubLab808Processor& processorIn) : processor(processorIn) {}

    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}

    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (! details.programChanged) return;
        std::thread stateReader([this] {
            juce::MemoryBlock state;
            processor.getStateInformation(state);
            stateReadSucceeded = ! state.isEmpty();
        });
        stateReader.join();
    }

    SubLab808Processor& processor;
    std::atomic<bool> stateReadSucceeded { false };
};

struct BlockingParameterListener final : juce::AudioProcessorParameter::Listener
{
    void parameterValueChanged(int, float) override
    {
        std::unique_lock lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [this] { return released; });
    }

    void parameterGestureChanged(int, bool) override {}

    void waitUntilEntered()
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [this] { return entered; });
    }

    void release()
    {
        {
            const std::lock_guard lock(mutex);
            released = true;
        }
        condition.notify_all();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released = false;
};

struct StateCapturingParameterListener final : juce::AudioProcessorParameter::Listener
{
    explicit StateCapturingParameterListener(SubLab808Processor& processorIn) : processor(processorIn) {}

    void parameterValueChanged(int, float) override
    {
        juce::MemoryBlock state;
        processor.getStateInformation(state);
        snapshots.push_back(std::move(state));
    }

    void parameterGestureChanged(int, bool) override {}

    SubLab808Processor& processor;
    std::vector<juce::MemoryBlock> snapshots;
};

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

float renderPeakAfterIdleOutputChange(bool changeWhileIdle)
{
    SubLab808Processor processor;
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "drive", 0.0f);
    setParameter(processor, "output", changeWhileIdle ? -24.0f : 6.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 2048); juce::MidiBuffer midi;
    if (changeWhileIdle) {
        setParameter(processor, "output", 6.0f);
        processor.processBlock(audio, midi); // more than the 20 ms smoothing period
    }

    audio.setSize(2, 512); audio.clear();
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 127), 0);
    processor.processBlock(audio, midi);
    return audio.getMagnitude(0, 0, 128);
}

std::pair<float, float> renderRepeatedNoteGateLevels()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "release", 0.01f);
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "drive", 0.0f);
    setParameter(processor, "oneshot", 0.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 110), 128);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 256);
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 2048);
    processor.processBlock(audio, midi);
    return { audio.getMagnitude(0, 1024, 512), audio.getMagnitude(0, 3584, 512) };
}

std::pair<float, float> renderCrossChannelNoteOffLevels()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "release", 0.01f);
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "drive", 0.0f);
    setParameter(processor, "oneshot", 0.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::noteOff(2, 60), 256);  // must not release channel 1
    midi.addEvent(juce::MidiMessage::noteOff(1, 60), 2048);
    processor.processBlock(audio, midi);
    return { audio.getMagnitude(0, 1024, 512), audio.getMagnitude(0, 3584, 512) };
}

struct ChannelControllerLevels {
    float afterAllNotesOff;
    float afterForeignAllSoundOff;
    float afterOwnerAllSoundOff;
};

ChannelControllerLevels renderChannelControllerLevels()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "release", 0.01f);
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "drive", 0.0f);
    setParameter(processor, "glide", 0.0f); setParameter(processor, "oneshot", 0.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 4096); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 48, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::noteOn(2, 52, (juce::uint8) 110), 128); // current voice
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 123, 0), 512);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 1024);
    midi.addEvent(juce::MidiMessage::controllerEvent(2, 120, 0), 2048);
    processor.processBlock(audio, midi);
    return {
        audio.getMagnitude(0, 640, 256),
        audio.getMagnitude(0, 1280, 512),
        audio.getMagnitude(0, 2048, 512)
    };
}

int renderChannelPitchCrossings(int bendChannel)
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 24000); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(2, 48, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(bendChannel, 16383), 0);
    processor.processBlock(audio, midi);
    return countPositiveCrossings(audio, 2048, 18000);
}

int renderCrossingsAfterResetAllControllers(bool sendReset)
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f);
    processor.prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> audio(2, 24000); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(2, 48, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(2, 16383), 0);
    if (sendReset) midi.addEvent(juce::MidiMessage::controllerEvent(2, 121, 0), 1024);
    processor.processBlock(audio, midi);
    return countPositiveCrossings(audio, 4096, 18000);
}

std::pair<int, int> renderBentGateReleaseCrossings(bool useAllNotesOff)
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "release", 0.5f);
    setParameter(processor, "punch", 0.0f); setParameter(processor, "click", 0.0f);
    setParameter(processor, "body", 0.0f); setParameter(processor, "drive", 0.0f);
    setParameter(processor, "oneshot", 0.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 40000); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(2, 48, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::pitchWheel(2, 16383), 0);
    if (useAllNotesOff)
        midi.addEvent(juce::MidiMessage::controllerEvent(2, 123, 0), 16384);
    else
        midi.addEvent(juce::MidiMessage::noteOff(2, 48), 16384);
    processor.processBlock(audio, midi);
    return { countPositiveCrossings(audio, 4096, 10000),
             countPositiveCrossings(audio, 20000, 10000) };
}

bool restoresOpenEditorSize()
{
    SubLab808Processor source;
    std::unique_ptr<juce::AudioProcessorEditor> sourceEditor(source.createEditor());
    sourceEditor->setSize(1000, 640);
    juce::MemoryBlock state; source.getStateInformation(state);

    SubLab808Processor destination;
    auto* destinationEditor = destination.createEditorAndMakeActive();
    if (destinationEditor == nullptr) return false;
    destination.setStateInformation(state.getData(), (int) state.getSize());
    const auto restored = destinationEditor->getWidth() == 1000 && destinationEditor->getHeight() == 640;
    if (! restored)
        std::fprintf(stderr, "editor restore: stored=%dx%d visible=%dx%d\n",
                     destination.getEditorSize().x, destination.getEditorSize().y,
                     destinationEditor->getWidth(), destinationEditor->getHeight());
    destination.editorBeingDeleted(destinationEditor);
    delete destinationEditor;
    return restored;
}

bool editorSizeSnapshotsStayCoherent()
{
    SubLab808Processor processor;
    processor.setEditorSize(820, 430);
    std::atomic<bool> finished { false };
    std::thread writer([&] {
        for (int i = 0; i < 200000; ++i)
            processor.setEditorSize((i & 1) == 0 ? 820 : 1100, (i & 1) == 0 ? 430 : 680);
        finished.store(true);
    });

    bool coherent = true;
    while (! finished.load()) {
        const auto size = processor.getEditorSize();
        if (! ((size.x == 820 && size.y == 430) || (size.x == 1100 && size.y == 680))) {
            coherent = false;
            break;
        }
    }
    writer.join();
    return coherent;
}

int renderOverflowFallbackCrossings()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f); setParameter(processor, "glide", 0.0f);
    setParameter(processor, "oneshot", 0.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 24000); juce::MidiBuffer midi;
    for (int note = 40; note <= 56; ++note)
        midi.addEvent(juce::MidiMessage::noteOn(1, note, (juce::uint8) 110), note - 40);
    midi.addEvent(juce::MidiMessage::noteOff(1, 40), 20);
    midi.addEvent(juce::MidiMessage::noteOn(1, 57, (juce::uint8) 110), 21);
    midi.addEvent(juce::MidiMessage::noteOff(1, 57), 22);
    processor.processBlock(audio, midi);
    return countPositiveCrossings(audio, 2048, 18000);
}

float renderOneShotPeakAtEightSeconds()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f); setParameter(processor, "oneshot", 1.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 127), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 36), 1);
    float finalPeak = 0.0f;
    constexpr int blocks = (8 * 48000) / 512;
    for (int block = 0; block < blocks; ++block) {
        audio.clear(); processor.processBlock(audio, midi); midi.clear();
        finalPeak = audio.getMagnitude(0, 0, audio.getNumSamples());
    }
    return finalPeak;
}

int renderOneShotCrossingsAfterNoteOff()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 0.0f);
    setParameter(processor, "click", 0.0f); setParameter(processor, "body", 0.0f);
    setParameter(processor, "drive", 0.0f); setParameter(processor, "glide", 0.0f);
    setParameter(processor, "oneshot", 1.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 24000); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 110), 0);
    midi.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 110), 512);
    midi.addEvent(juce::MidiMessage::noteOff(1, 64), 1024);
    processor.processBlock(audio, midi);
    return countPositiveCrossings(audio, 2048, 18000);
}

float renderWorstCasePeakAtReportedTail()
{
    SubLab808Processor processor;
    setParameter(processor, "decay", 4.0f); setParameter(processor, "punch", 48.0f);
    setParameter(processor, "click", 100.0f); setParameter(processor, "body", 100.0f);
    setParameter(processor, "drive", 24.0f); setParameter(processor, "tone", 80.0f);
    setParameter(processor, "velocity", 100.0f); setParameter(processor, "output", 6.0f);
    setParameter(processor, "oneshot", 1.0f);
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> audio(2, 512); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8) 127), 0);
    midi.addEvent(juce::MidiMessage::noteOff(1, 36), 1);
    const auto blocks = (int) std::ceil(processor.getTailLengthSeconds() * 48000.0 / 512.0);
    float finalPeak = 0.0f;
    for (int block = 0; block < blocks; ++block) {
        audio.clear(); processor.processBlock(audio, midi); midi.clear();
        finalPeak = audio.getMagnitude(0, 0, audio.getNumSamples());
    }
    return finalPeak;
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

    // A re-entrant host/listener edit during preset application must not leave the
    // UI claiming an untouched factory preset when the final parameter state differs.
    {
        SubLab808Processor interleaved;
        interleaved.setCurrentProgram(0);
        auto* decay = interleaved.parameters.getParameter("decay");
        if (decay == nullptr) return 29;
        ReentrantPresetEdit edit(*decay, 3.7f);
        decay->addListener(&edit);
        interleaved.setCurrentProgram(1);
        decay->removeListener(&edit);
        if (std::abs(interleaved.parameters.getRawParameterValue("decay")->load() - 3.7f) > 0.01f
            || ! interleaved.isPresetModified()) return 29;
    }

    // A program requested from inside an older parameter notification is deferred
    // until that listener traversal returns, then applied before the outer call ends.
    {
        SubLab808Processor queued;
        queued.setCurrentProgram(0);
        auto* decay = queued.parameters.getParameter("decay");
        if (decay == nullptr) return 30;
        ReentrantProgramSelection selection(queued, 2);
        decay->addListener(&selection);
        queued.setCurrentProgram(1);
        decay->removeListener(&selection);
        if (selection.observedProgramAfterCall == 2
            || queued.getCurrentProgram() != 2 || queued.isPresetModified()) return 30;
        for (const auto* id : { "decay", "release", "punch", "pitchdecay", "glide", "tune",
                                "body", "click", "drive", "tone", "velocity", "output" })
            if (std::abs(queued.parameters.getRawParameterValue(id)->load()
                         - queued.getFactoryPresetValue(2, id)) > 0.0005f) return 30;
        if ((queued.parameters.getRawParameterValue("oneshot")->load() >= 0.5f)
            != (queued.getFactoryPresetValue(2, "oneshot") >= 0.5f)) return 30;
    }

    // A host that synchronously echoes ProgramChanged back as setCurrentProgram(current)
    // must be coalesced instead of recursively notifying forever.
    {
        SubLab808Processor echoed;
        EchoingProgramHost host(echoed);
        echoed.addListener(&host);
        echoed.setCurrentProgram(1);
        echoed.removeListener(&host);
        if (host.programNotifications != 1 || echoed.getCurrentProgram() != 1
            || echoed.isPresetModified()) return 33;
    }

    // Re-entrant program requests retain FIFO order. In particular, a legitimate
    // A -> B -> A cascade must not lose the final A while breaking echo cycles.
    {
        SubLab808Processor cascaded;
        CascadingProgramHost host(cascaded);
        cascaded.addListener(&host);
        cascaded.setCurrentProgram(1);
        cascaded.removeListener(&host);
        if (host.programs != std::vector<int> { 1, 2, 1 }
            || cascaded.getCurrentProgram() != 1
            || cascaded.isPresetModified()
            || ! matchesFactoryProgram(cascaded, 1)) return 40;
    }


    // No internal program lock may be held while ProgramChanged invokes the host:
    // a host that synchronously waits for cross-thread state capture must terminate.
    {
        SubLab808Processor callbackSafe;
        CrossThreadStateHost host(callbackSafe);
        callbackSafe.addListener(&host);
        callbackSafe.setCurrentProgram(1);
        callbackSafe.removeListener(&host);
        if (! host.stateReadSucceeded.load()) return 35;
    }

    // An independent cross-thread setter that overlaps a parameter callback must
    // wait and then apply synchronously; callback timing may never drop the request.
    {
        SubLab808Processor serialized;
        auto* decay = serialized.parameters.getParameter("decay");
        if (decay == nullptr) return 37;
        BlockingParameterListener blocker;
        decay->addListener(&blocker);
        std::thread firstWriter([&] { serialized.setCurrentProgram(1); });
        blocker.waitUntilEntered();
        juce::MemoryBlock snapshotDuringCallback;
        serialized.getStateInformation(snapshotDuringCallback);

        std::atomic<bool> secondStarted { false }, secondReturned { false };
        std::thread secondWriter([&] {
            secondStarted.store(true);
            serialized.setCurrentProgram(2);
            secondReturned.store(true);
        });
        while (! secondStarted.load()) std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const auto returnedBeforeRelease = secondReturned.load();
        blocker.release();
        firstWriter.join();
        secondWriter.join();
        decay->removeListener(&blocker);
        SubLab808Processor restoredSnapshot;
        restoredSnapshot.setStateInformation(snapshotDuringCallback.getData(),
                                              (int) snapshotDuringCallback.getSize());
        if (returnedBeforeRelease || ! secondReturned.load()
            || serialized.getCurrentProgram() != 2
            || ! matchesFactoryProgram(serialized, 2)
            || restoredSnapshot.getCurrentProgram() != 0
            || ! matchesFactoryProgram(restoredSnapshot, 0)) return 37;
    }

    // State capture from every in-flight preset parameter notification must return
    // the previous complete snapshot, never a partially published new preset.
    {
        SubLab808Processor snapshotSource;
        snapshotSource.setCurrentProgram(0);
        StateCapturingParameterListener capture(snapshotSource);
        std::vector<juce::RangedAudioParameter*> listenedParameters;
        for (const auto* id : { "decay", "release", "punch", "pitchdecay", "glide", "tune",
                                "body", "click", "drive", "tone", "velocity", "output", "oneshot" })
            if (auto* parameter = snapshotSource.parameters.getParameter(id)) {
                parameter->addListener(&capture);
                listenedParameters.push_back(parameter);
            }
        snapshotSource.setCurrentProgram(1);
        for (auto* parameter : listenedParameters) parameter->removeListener(&capture);
        if (capture.snapshots.size() != listenedParameters.size()) return 38;
        for (const auto& snapshot : capture.snapshots) {
            SubLab808Processor restored;
            restored.setStateInformation(snapshot.getData(), (int) snapshot.getSize());
            if (restored.getCurrentProgram() != 0 || restored.isPresetModified()
                || ! matchesFactoryProgram(restored, 0)) return 38;
        }
        juce::MemoryBlock committed;
        snapshotSource.getStateInformation(committed);
        SubLab808Processor restored;
        restored.setStateInformation(committed.getData(), (int) committed.getSize());
        if (restored.getCurrentProgram() != 1 || restored.isPresetModified()
            || ! matchesFactoryProgram(restored, 1)) return 38;
    }

    // Re-selecting a factory preset also repairs a stale Custom marker when a user
    // has manually returned every parameter to the exact factory values.
    {
        SubLab808Processor repaired;
        repaired.setCurrentProgram(0);
        setParameter(repaired, "decay", 2.0f);
        setParameter(repaired, "decay", repaired.getFactoryPresetValue(0, "decay"));
        if (! repaired.isPresetModified()) return 36;
        repaired.setCurrentProgram(0);
        if (repaired.isPresetModified() || ! matchesFactoryProgram(repaired, 0)) return 36;
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
    // Exercise every channel/note key plus repeated move-to-newest operations. The
    // intrusive order must retain all 2048 keys and finish without a stuck voice.
    {
        SubLab808Processor many;
        setParameter(many, "oneshot", 0.0f); setParameter(many, "release", 0.01f); setParameter(many, "decay", 4.0f);
        many.prepareToPlay(48000.0, 512);
        juce::AudioBuffer<float> manyAudio(2, 48000); juce::MidiBuffer manyMidi;
        int samplePosition = 0;
        for (int channel = 1; channel <= 16; ++channel)
            for (int note = 0; note < 128; ++note)
                manyMidi.addEvent(juce::MidiMessage::noteOn(channel, note, (juce::uint8) 100), samplePosition++);
        for (int retrigger = 0; retrigger < 128; ++retrigger) {
            manyMidi.addEvent(juce::MidiMessage::noteOn(1, 0, (juce::uint8) 100), 4096 + retrigger * 2);
            manyMidi.addEvent(juce::MidiMessage::noteOff(1, 0), 4097 + retrigger * 2);
        }
        samplePosition = 8192;
        for (int channel = 1; channel <= 16; ++channel)
            for (int note = 0; note < 128; ++note)
                manyMidi.addEvent(juce::MidiMessage::noteOff(channel, note), samplePosition++);
        many.processBlock(manyAudio, manyMidi);
        if (manyAudio.getMagnitude(0, 40000, 8000) > 0.001f) return 17;
    }
    {
        const auto at48 = renderLevelAtHalfSecond(48000.0), at96 = renderLevelAtHalfSecond(96000.0);
        if (at48 < 0.01f || std::abs(at48 - at96) > 0.05f * at48) return 14;
    }
    {
        const auto settled = renderPeakAfterIdleOutputChange(false);
        const auto changedWhileIdle = renderPeakAfterIdleOutputChange(true);
        if (settled < 0.1f || changedWhileIdle < settled * 0.9f) return 15;
    }
    {
        const auto [afterFirstOff, afterSecondOff] = renderRepeatedNoteGateLevels();
        if (afterFirstOff < 0.05f || afterSecondOff > afterFirstOff * 0.1f) return 16;
    }
    {
        // SysEx is irrelevant to this synth and must be skipped without preventing a channel
        // message at the same sample position from reaching the voice.
        SubLab808Processor sysex;
        sysex.prepareToPlay(48000.0, 512);
        juce::AudioBuffer<float> sysexAudio(2, 512);
        juce::MidiBuffer sysexMidi;
        std::array<juce::uint8, 64> payload {};
        for (size_t index = 0; index < payload.size(); ++index)
            payload[index] = static_cast<juce::uint8>(index & 0x7f);
        sysexMidi.addEvent(juce::MidiMessage::createSysExMessage(payload.data(),
                                                                 static_cast<int>(payload.size())), 0);
        sysexMidi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(127)), 0);

#if defined(__APPLE__) && !SUBLAB808_ADDRESS_SANITIZER
        // Keep the probe honest: the pinned JUCE implementation must register a long-message
        // allocation before the corrected processBlock path is required to register none.
        allocationProbe::begin();
        const auto calibrationMessage = (*sysexMidi.begin()).getMessage();
        const auto calibrationAllocations = allocationProbe::end();
        if (calibrationMessage.getRawDataSize() <= 8 || calibrationAllocations == 0)
            return 44;
#endif

        juce::AudioBuffer<float> warmAudio(2, 512);
        juce::MidiBuffer warmMidi;
        warmMidi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(100)), 0);
        warmMidi.addEvent(juce::MidiMessage::controllerEvent(1, 120, 0), 256);
        sysex.processBlock(warmAudio, warmMidi);

#if defined(__APPLE__) && !SUBLAB808_ADDRESS_SANITIZER
        allocationProbe::begin();
#endif
        sysex.processBlock(sysexAudio, sysexMidi);
#if defined(__APPLE__) && !SUBLAB808_ADDRESS_SANITIZER
        const auto realtimeAllocations = allocationProbe::end();
        if (realtimeAllocations != 0)
        {
            std::fprintf(stderr, "SysEx processBlock allocations: %zu\n", realtimeAllocations);
            return 43;
        }
#endif
        if (sysexAudio.getMagnitude(0, 0, sysexAudio.getNumSamples()) < 0.01f) return 42;
    }
    {
        const auto [afterWrongChannelOff, afterOwnerOff] = renderCrossChannelNoteOffLevels();
        if (afterWrongChannelOff < 0.05f || afterOwnerOff > afterWrongChannelOff * 0.1f) return 23;
    }
    {
        const auto levels = renderChannelControllerLevels();
        if (levels.afterAllNotesOff < 0.05f || levels.afterForeignAllSoundOff < 0.05f
            || levels.afterOwnerAllSoundOff > 1.0e-7f) return 24;
    }
    {
        const auto foreignBend = renderChannelPitchCrossings(1);
        const auto ownerBend = renderChannelPitchCrossings(2);
        const auto expectedUnbent = (int) std::round(juce::MidiMessage::getMidiNoteInHertz(48) * 18000.0 / 48000.0);
        if (std::abs(foreignBend - expectedUnbent) > 1 || ownerBend <= foreignBend + 2) return 25;
    }
    {
        const auto bent = renderCrossingsAfterResetAllControllers(false);
        const auto reset = renderCrossingsAfterResetAllControllers(true);
        if (reset >= bent - 2) return 31;
    }
    for (const auto useAllNotesOff : { false, true }) {
        const auto [beforeRelease, duringRelease] = renderBentGateReleaseCrossings(useAllNotesOff);
        if (std::abs(beforeRelease - duringRelease) > 1) {
            std::fprintf(stderr, "bent release crossings (%s): %d before, %d during\n",
                         useAllNotesOff ? "CC123" : "note-off", beforeRelease, duringRelease);
            return 27;
        }
    }
    {
        const auto crossings = renderOverflowFallbackCrossings();
        const auto expected = (int) std::round(juce::MidiMessage::getMidiNoteInHertz(56) * 18000.0 / 48000.0);
        if (std::abs(crossings - expected) > 1) {
            std::fprintf(stderr, "overflow fallback crossings: %d (expected %d)\n", crossings, expected);
            return 17;
        }
    }
    {
        const auto tail = processor.getTailLengthSeconds();
        if (! std::isfinite(tail) || tail < 46.1 || tail > 47.1
            || renderOneShotPeakAtEightSeconds() < 0.01f
            || renderWorstCasePeakAtReportedTail() > 1.0e-5f) return 18;
    }
    {
        const auto crossings = renderOneShotCrossingsAfterNoteOff();
        const auto expected = (int) std::round(juce::MidiMessage::getMidiNoteInHertz(64) * 18000.0 / 48000.0);
        if (std::abs(crossings - expected) > 1) {
            std::fprintf(stderr, "one-shot crossings: %d (expected %d)\n", crossings, expected);
            return 19;
        }
    }
    processor.setCurrentProgram(3); setParameter(processor, "drive", 12.0f);
    if (processor.getProgramName(3) != "Dirty Trunk" || ! processor.isPresetModified()) return 20;
    {
        const auto frozenLegacyFactoryState = legacyDirtyTrunkStateFixture();
        if (frozenLegacyFactoryState.isEmpty()) return 21;
        processor.setCurrentProgram(0);
        auto* restoreDecay = processor.parameters.getParameter("decay");
        if (restoreDecay == nullptr) return 21;
        ReentrantProgramSelection restoreCallback(processor, 2);
        restoreDecay->addListener(&restoreCallback);
        processor.setStateInformation(frozenLegacyFactoryState.getData(), (int) frozenLegacyFactoryState.getSize());
        restoreDecay->removeListener(&restoreCallback);
        if (processor.getCurrentProgram() != 3 || processor.isPresetModified()
            || restoreCallback.armed || restoreCallback.observedProgramAfterCall != 0
            || std::abs(processor.parameters.getRawParameterValue("drive")->load() - 16.0f) > 0.01f) return 21;

        processor.setCurrentProgram(3); setParameter(processor, "drive", 12.0f);
        juce::MemoryBlock editedState; processor.getStateInformation(editedState);
        const auto legacyEditedState = withoutStateProperty(editedState, "presetModified");
        processor.setCurrentProgram(0);
        processor.setStateInformation(legacyEditedState.getData(), (int) legacyEditedState.getSize());
        if (! processor.isPresetModified()) return 21;

        processor.setCurrentProgram(3);
        juce::MemoryBlock factoryState; processor.getStateInformation(factoryState);
        const auto legacyFactoryState = withoutStateProperty(factoryState, "presetModified");
        processor.setCurrentProgram(0);
        processor.setStateInformation(legacyFactoryState.getData(), (int) legacyFactoryState.getSize());
        if (processor.isPresetModified()) return 22;

        processor.setCurrentProgram(3);
        setParameter(processor, "drive", 13.7f);
        juce::MemoryBlock mismatchedState; processor.getStateInformation(mismatchedState);
        const auto falselyCleanState = withStateProperty(mismatchedState, "presetModified", false);
        processor.setCurrentProgram(0);
        processor.setStateInformation(falselyCleanState.getData(), (int) falselyCleanState.getSize());
        if (! processor.isPresetModified()) return 32;
    }
    if (! restoresOpenEditorSize()) return 26;
    if (! editorSizeSnapshotsStayCoherent()) return 28;

    // Program application and state restore share one control-state transaction;
    // concurrent callers may win in either order, but may never leave a hybrid.
    {
        SubLab808Processor serialized;
        const auto restoreState = legacyDirtyTrunkStateFixture();
        std::atomic<bool> start { false };
        std::vector<juce::MemoryBlock> concurrentSnapshots;
        std::thread programs([&] {
            while (! start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i) serialized.setCurrentProgram((i & 1) == 0 ? 1 : 2);
        });
        std::thread restores([&] {
            while (! start.load()) std::this_thread::yield();
            for (int i = 0; i < 100; ++i)
                serialized.setStateInformation(restoreState.getData(), (int) restoreState.getSize());
        });
        std::thread stateReader([&] {
            while (! start.load()) std::this_thread::yield();
            for (int i = 0; i < 128; ++i) {
                juce::MemoryBlock snapshot;
                serialized.getStateInformation(snapshot);
                concurrentSnapshots.push_back(std::move(snapshot));
            }
        });
        start.store(true);
        programs.join();
        restores.join();
        stateReader.join();
        const auto finalProgram = serialized.getCurrentProgram();
        if (! juce::isPositiveAndBelow(finalProgram, serialized.getNumPrograms())
            || ! matchesFactoryProgram(serialized, finalProgram)
            || serialized.isPresetModified()) return 34;
        for (const auto& snapshot : concurrentSnapshots) {
            SubLab808Processor restored;
            restored.setStateInformation(snapshot.getData(), (int) snapshot.getSize());
            const auto savedProgram = restored.getCurrentProgram();
            if (! juce::isPositiveAndBelow(savedProgram, restored.getNumPrograms())
                || ! matchesFactoryProgram(restored, savedProgram)
                || restored.isPresetModified()) return 41;
        }
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
