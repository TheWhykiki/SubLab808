#pragma once
#include "PresetLibrary.h"
#include <JuceHeader.h>
#include <deque>
#include <mutex>

class SubLab808Processor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener,
                                 private juce::AsyncUpdater
{
public:
    explicit SubLab808Processor(juce::File presetStorage = {});
    ~SubLab808Processor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "SubLab808"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 47.0; }
    int getNumPrograms() override;
    float getFactoryPresetValue(int index, const juce::String& parameterId) const;
    int getCurrentProgram() override { return currentProgram.load(); }
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState parameters;
    wk::PresetLibrary presets;
    std::atomic<float> outputMeter { 0.0f };
    juce::Point<int> getEditorSize() const noexcept;
    void setEditorSize(int width, int height) noexcept;
    bool isPresetModified() const { return presetModified.load(); }
#if JUCE_STANDALONE_APPLICATION
    // Console-test access to the same public JUCE continuation mechanism. This is
    // not part of the VST3 processor interface and never changes parameter values.
    void servicePendingStateNotificationsForTesting() { handleUpdateNowIfNeeded(); }
    bool hasPendingStateNotificationsForTesting() const { return isUpdatePending(); }
    std::function<void()> beforeQueuedProgramCommitForTesting;
#endif

private:
    struct ControlOperation
    {
        enum class Kind { program, state };
        Kind kind = Kind::program;
        int programIndex = 0;
        bool notifyHost = true;
        bool queuedProgram = false;
        uint64_t restoreEpoch = 0;
        juce::ValueTree state;
    };

    struct RuntimeParameters
    {
        float decay = 0.8f, release = 0.12f, punch = 18.0f, pitchDecay = 0.045f;
        float glide = 0.03f, tune = 0.0f, body = 18.0f, click = 12.0f;
        float drive = 5.0f, tone = 5000.0f, velocity = 80.0f, output = -3.0f;
        bool oneShot = true;
        uint64_t clickSequenceGeneration = 0;
    };

    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void submitControlOperation(ControlOperation);
    void commitControlOperation(ControlOperation);
    void drainStateNotifications();
    void handleAsyncUpdate() override;
    void applyRestoredEditorSize();
    float readParameter(size_t index) const noexcept;
    RuntimeParameters readRuntimeParameters() const;
    void refreshRuntimeParameters() noexcept;
    void triggerNote(int channel, int note, float newVelocity);
    void releaseNote(int channel, int note);
    void allNotesOff(int channel);
    void allSoundOff(int channel);
    void removeHeldKeyFromOrder(int key);
    void appendHeldKeyToOrder(int key);
    void clearHeldKeys();
    static int keyForMidiMessage(int channel, int note) noexcept;
    void resetSound();
    float renderSample();
    bool parametersMatchProgram(int index);
    static uint64_t packEditorSize(int width, int height) noexcept;
    void parameterChanged(const juce::String&, float) override;
    double sampleRate = 44100.0, phase = 0.0, currentHz = 55.0, targetHz = 55.0;
    float amp = 0.0f, pitchEnv = 0.0f, filterState = 0.0f, velocity = 1.0f;
    float click = 0.0f, clickCoef = 0.965f;
    juce::SmoothedValue<float> ampCoef, pitchCoef, glideCoef, releaseCoef;
    juce::SmoothedValue<float> drive, outputGain, filterCoef, body, tuneSemitones;
    static constexpr uint32_t initialNoiseSeed = 0x6d2b79f5u;
    uint32_t noiseState = initialNoiseSeed;
    std::atomic<uint64_t> clickSequenceGeneration { 0 };
    static constexpr uint64_t editorRestorePendingMask = uint64_t { 1 } << 63;
    std::atomic<uint64_t> editorSize { packEditorSize(860, 520) };
    std::atomic<int> currentProgram { 0 };
    std::atomic<unsigned> internalParameterChangeDepth { 0 };
    std::atomic<bool> presetModified { false };
    static constexpr std::array<const char*, 13> parameterIds {
        "decay", "release", "punch", "pitchdecay", "glide", "tune", "body",
        "click", "drive", "tone", "velocity", "output", "oneshot"
    };
    std::array<juce::RangedAudioParameter*, parameterIds.size()> rangedParameters {};
    // The commit gate never calls APVTS, listeners, the host, or an editor.
    std::mutex controlMutex;
    juce::ValueTree stateExtensions { "PARAMETERS" }; // immutable between commits
    std::deque<ControlOperation> pendingProgramOperations;
    uint64_t restoreEpoch = 0; // protected by controlMutex; invalidates even a locally dequeued old request
    bool committedProgramNotification = false, committedRestoreNotification = false;
    std::atomic<uint64_t> controlGeneration { 0 }, notifiedGeneration { 0 };
    std::atomic<juce::Thread::ThreadID> notificationOwner { nullptr };
    std::atomic<bool> notifyingRestore { false };
    // All packet reads/writes, including JUCE's standard parameter atomics and
    // the Click generation, participate in the same sequentially consistent order.
    std::atomic<uint64_t> parameterTransactionSequence { 0 };
    RuntimeParameters runtimeParameters;
    static constexpr float amplitudeSilenceThreshold = 1.0e-5f;
    static constexpr int midiChannelCount = 16;
    static constexpr int midiNoteCount = 128;
    static constexpr int midiKeyCount = midiChannelCount * midiNoteCount;
    static constexpr unsigned maxGenerationsPerDrain = 128;
    static_assert(std::atomic<float>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);
    std::array<uint32_t, midiKeyCount> heldKeyCounts {};
    std::array<int, midiKeyCount> previousHeldKeys {}, nextHeldKeys {};
    std::array<juce::SmoothedValue<float>, midiChannelCount> channelBendSemitones;
    int numHeldKeys = 0;
    int oldestHeldKey = -1, newestHeldKey = -1;
    int currentKey = -1;
    bool gateReleased = false;
    bool active = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubLab808Processor)
};
