#pragma once
#include <JuceHeader.h>

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
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    void triggerNote(int channel, int note, float newVelocity);
    void releaseNote(int channel, int note);
    void allNotesOff(int channel);
    void allSoundOff(int channel);
    void removeHeldKeyFromOrder(int key);
    void clearHeldKeys();
    static int keyForMidiMessage(int channel, int note) noexcept;
    void resetSound();
    float renderSample();
    bool parametersMatchProgram(int index);
    static uint64_t packEditorSize(int width, int height) noexcept;
    void parameterChanged(const juce::String&, float) override;
    double sampleRate = 44100.0, phase = 0.0, currentHz = 55.0, targetHz = 55.0;
    float amp = 0.0f, pitchEnv = 0.0f, filterState = 0.0f, velocity = 1.0f, tuneSemitones = 0.0f;
    float click = 0.0f, clickCoef = 0.965f;
    juce::SmoothedValue<float> ampCoef, pitchCoef, glideCoef, releaseCoef;
    juce::SmoothedValue<float> drive, outputGain, filterCoef, body;
    uint32_t noiseState = 0x6d2b79f5u;
    std::atomic<uint64_t> editorSize { packEditorSize(860, 520) };
    std::atomic<int> currentProgram { 0 };
    std::atomic<bool> presetModified { false }, applyingPreset { false };
    static constexpr float amplitudeSilenceThreshold = 1.0e-5f;
    static constexpr int midiChannelCount = 16;
    static constexpr int midiNoteCount = 128;
    static constexpr int midiKeyCount = midiChannelCount * midiNoteCount;
    std::array<int, midiKeyCount> heldKeys {};
    std::array<uint16_t, midiKeyCount> heldKeyCounts {};
    std::array<float, midiChannelCount> channelBendSemitones {};
    int numHeldKeys = 0;
    int currentKey = -1;
    bool gateReleased = false;
    bool active = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SubLab808Processor)
};
