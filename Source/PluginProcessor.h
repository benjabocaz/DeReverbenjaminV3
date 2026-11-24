#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

class DeReverbenjaminV3Processor : public juce::AudioProcessor
{
public:
    DeReverbenjaminV3Processor();
    ~DeReverbenjaminV3Processor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> currentGainReduction { 1.0f };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int hopSize = fftSize / 4;
    static constexpr int numBins = fftSize / 2 + 1;

    struct ChannelState
    {
        std::array<float, fftSize> inputBuffer{};
        std::array<float, fftSize> outputAccum{};
        std::array<float, hopSize> outputReady{};
        std::array<float, fftSize * 2> fftData{};

        std::array<float, numBins> prevPower{};
        std::array<float, numBins> reverbEst{};
        std::array<float, numBins> rawGain{};
        std::array<float, numBins> smoothGain{};

        // Señal seca alineada en fase con la FFT
        std::array<float, fftSize> dryDelay{};
        int dryWritePos = 0;

        // Filtros internos de Etapa 3 (resíntesis y bandas)
        float lowLp = 0.0f;
        float midLoLp = 0.0f;
        float midHiLp = 0.0f;
        float airLp = 0.0f;
        float dryLowLp = 0.0f;

        // Generador de ruido filtrado para resíntesis de aire/bordona
        uint32_t rngState = 2463534242u;
        float noiseHpState = 0.0f;
        float noiseLpState = 0.0f;

        // Bloqueador de DC para saturación asimétrica (armónicos pares)
        float dcPrevIn = 0.0f;
        float dcPrevOut = 0.0f;

        int hopIndex = 0;
        float lastAvgGain = 1.0f;

        float nextWhiteNoise()
        {
            uint32_t x = rngState;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            rngState = x;
            return (static_cast<float>(x & 0x00FFFFFFu) / 8388608.0f) - 1.0f;
        }

        void reset(uint32_t seed)
        {
            inputBuffer.fill(0.0f);
            outputAccum.fill(0.0f);
            outputReady.fill(0.0f);
            fftData.fill(0.0f);
            prevPower.fill(0.0f);
            reverbEst.fill(0.0f);
            rawGain.fill(1.0f);
            smoothGain.fill(1.0f);
            dryDelay.fill(0.0f);
            dryWritePos = 0;
            lowLp = 0.0f;
            midLoLp = 0.0f;
            midHiLp = 0.0f;
            airLp = 0.0f;
            dryLowLp = 0.0f;
            rngState = seed;
            noiseHpState = 0.0f;
            noiseLpState = 0.0f;
            dcPrevIn = 0.0f;
            dcPrevOut = 0.0f;
            hopIndex = 0;
            lastAvgGain = 1.0f;
        }
    };

    void processFrame(ChannelState& state, float amount, float roomDecay, float roomFocus, float smoothness, int mode);

    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { fftSize, juce::dsp::WindowingFunction<float>::hann, false };
    std::vector<ChannelState> channels;

    // Etapa 2: Doble envolvente (De-Tail)
    float fastEnv = 0.0f;
    float slowEnv = 0.0f;
    float tailSmoothGain = 1.0f;

    // Etapa 3: Detector de transientes suavizado (cero distorsión)
    float dryTransFast = 0.0f;
    float dryTransSlow = 0.0f;
    float transSmooth = 0.0f;

    // Etapa 3: Sintetizador de sub-graves (DRUMS) y Opto-Glue / Air-Gate (VOCAL)
    float lowTransFast = 0.0f;
    float lowTransSlow = 0.0f;
    float subEnv = 0.0f;
    float subPhase = 0.0f;
    float vocalOptoEnv = 0.0f;
    float airGateEnv = 0.0f;

    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeReverbenjaminV3Processor)
};
