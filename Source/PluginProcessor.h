#pragma once
#include <JuceHeader.h>
#include <condition_variable>
#include <deque>
#include <mutex>

class SubLab808Processor final : public juce::AudioProcessor,
                                 private juce::AudioProcessorValueTreeState::Listener
{
public:
    SubLab808Processor();
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
    std::atomic<float> outputMeter { 0.0f };
    juce::Point<int> getEditorSize() const noexcept;
    void setEditorSize(int width, int height) noexcept;
    bool isPresetModified() const { return presetModified.load(); }

private:
    struct ControlOperation
    {
        enum class Kind { program, state };
        Kind kind = Kind::program;
        int programIndex = 0;
        bool notifyHost = true;
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
    void performControlOperation(const ControlOperation&);
    bool applyProgramNow(int index);
    bool applyStateNow(const juce::ValueTree&);
    juce::MemoryBlock createStateSnapshot();
    void beginStateTransaction();
    void endStateTransaction();
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
    std::atomic<uint64_t> editorSize { packEditorSize(860, 520) };
    std::atomic<int> currentProgram { 0 };
    std::atomic<unsigned> internalParameterChangeDepth { 0 };
    std::atomic<bool> presetModified { false };
    std::atomic<bool> stateRestoreActive { false };
    std::mutex controlMutex;
    std::condition_variable controlCondition;
    juce::Thread::ThreadID controlOwner = nullptr;
    std::deque<ControlOperation> pendingControlOperations;
    unsigned controlOperationBudget = 0;
    juce::CriticalSection stateSnapshotLock;
    juce::MemoryBlock lastCommittedState;
    bool stateTransactionActive = false;
    uint64_t stateSnapshotGeneration = 0;
    std::atomic<uint64_t> parameterTransactionSequence { 0 };
    RuntimeParameters runtimeParameters;
    static constexpr float amplitudeSilenceThreshold = 1.0e-5f;
    static constexpr int midiChannelCount = 16;
    static constexpr int midiNoteCount = 128;
    static constexpr int midiKeyCount = midiChannelCount * midiNoteCount;
    static constexpr unsigned maxControlOperationsPerDrain = 64;
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
