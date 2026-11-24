#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

DeReverbenjaminV3Processor::DeReverbenjaminV3Processor()
    : AudioProcessor(BusesProperties()
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout())
{
}

juce::AudioProcessorValueTreeState::ParameterLayout DeReverbenjaminV3Processor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Selector de Fuente (0: DRUMS, 1: VOCAL, 2: GTR/BASS, 3: KEYS)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{"mode", 1}, "Source Mode",
        juce::StringArray{"DRUMS", "VOCAL", "GTR/BASS", "KEYS"}, 0));

    // Etapa 1: Spectral De-Room
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"roomAmount", 1}, "De-Room",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.60f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"focus", 1}, "Ambiance",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.55f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"decay", 1}, "Room Decay",
        juce::NormalisableRange<float>(0.05f, 0.96f, 0.01f), 0.55f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"smooth", 1}, "Smooth",
        juce::NormalisableRange<float>(0.0f, 0.95f, 0.01f), 0.55f));

    // Etapa 2: Tail De-Reverb & Output
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"tailAmount", 1}, "De-Tail",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.35f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"sensitivity", 1}, "Sensitivity",
        juce::NormalisableRange<float>(0.1f, 2.0f, 0.01f), 1.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"release", 1}, "Release",
        juce::NormalisableRange<float>(20.0f, 400.0f, 1.0f), 100.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"output", 1}, "Output",
        juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f), 0.0f));

    // Etapa 3: Reconstruct & Resynth (controles dinámicos según SOURCE)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"res1", 1}, "Resynth 1",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.30f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"res2", 1}, "Resynth 2",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.25f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"res3", 1}, "Resynth 3",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.20f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{"mix", 1}, "Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f));

    return { params.begin(), params.end() };
}

void DeReverbenjaminV3Processor::prepareToPlay(double sampleRate, int)
{
    currentSampleRate = sampleRate;
    setLatencySamples(fftSize);

    channels.resize(static_cast<size_t>(std::max(1, getTotalNumInputChannels())));
    for (size_t c = 0; c < channels.size(); ++c)
        channels[c].reset(2463534242u + static_cast<uint32_t>(c * 9876543u));

    fastEnv = 0.0f;
    slowEnv = 0.0f;
    tailSmoothGain = 1.0f;
    dryTransFast = 0.0f;
    dryTransSlow = 0.0f;
    transSmooth = 0.0f;
    lowTransFast = 0.0f;
    lowTransSlow = 0.0f;
    subEnv = 0.0f;
    subPhase = 0.0f;
    vocalOptoEnv = 0.0f;
    airGateEnv = 0.0f;
    currentGainReduction.store(1.0f);
}

bool DeReverbenjaminV3Processor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void DeReverbenjaminV3Processor::processFrame(ChannelState& state, float amount, float roomDecay,
                                              float roomFocus, float smoothness, int mode)
{
    std::copy(state.inputBuffer.begin(), state.inputBuffer.end(), state.fftData.begin());
    std::fill(state.fftData.begin() + fftSize, state.fftData.end(), 0.0f);
    window.multiplyWithWindowingTable(state.fftData.data(), fftSize);

    fft.performRealOnlyForwardTransform(state.fftData.data(), true);

    const float binHz = static_cast<float>(currentSampleRate) / static_cast<float>(fftSize);

    // Piso y agresividad específicos por instrumento
    float modeFloor = 0.02f;               // DRUMS: secado profundo intacto
    if (mode == 1) modeFloor = 0.11f;      // VOCAL: evita que palabras suaves desaparezcan
    else if (mode == 2) modeFloor = 0.04f; // GTR/BASS
    else if (mode == 3) modeFloor = 0.06f; // KEYS

    const float overSub = (mode == 1)
        ? (0.50f + roomFocus * 2.05f)      // VOCAL: más preciso, menos agresivo con sílabas
        : (0.65f + roomFocus * 2.85f);     // DRUMS/otros: igual que v3

    const float minGain = std::max(modeFloor, 1.0f - (amount * (mode == 1 ? 0.89f : 0.98f)));
    const float timeSmooth = 0.24f + smoothness * 0.66f;

    for (int k = 0; k < numBins; ++k)
    {
        const float freq = k * binHz;
        const float re = state.fftData[2 * k];
        const float im = state.fftData[2 * k + 1];
        const float power = re * re + im * im;

        // En VOCAL: si entra una sílaba o consonante nueva, soltamos rápido el estimado de sala anterior
        const bool vocalOnset = (mode == 1) && (power > state.prevPower[k] * 1.55f);
        const float effectiveDecay = vocalOnset ? (roomDecay * 0.52f) : roomDecay;

        state.reverbEst[k] = std::max(effectiveDecay * state.reverbEst[k],
                                      0.32f * state.reverbEst[k] + 0.68f * state.prevPower[k]);
        state.prevPower[k] = power;

        // Protección espectral específica por instrumento
        float bandWeight = 1.0f;
        if (mode == 0 && freq < 95.0f)
            bandWeight = 0.55f; // DRUMS: cuida sub-bombo
        else if (mode == 1)
        {
            if (vocalOnset && freq >= 2200.0f && freq <= 7000.0f)
                bandWeight = 0.42f; // VOCAL: protege consonantes (p, t, k, s) en el ataque
            else if (freq >= 170.0f && freq <= 3400.0f)
                bandWeight = 0.72f; // VOCAL: protege cuerpo e inteligibilidad de la voz
        }
        else if (mode == 2 && freq >= 90.0f && freq <= 2000.0f)
            bandWeight = 0.80f; // GTR/BASS
        else if (mode == 3 && freq < 220.0f)
            bandWeight = 0.72f; // KEYS

        const float estRoom = state.reverbEst[k] * overSub * bandWeight;
        const float cleanPower = std::max(0.0f, power - estRoom);
        const float wiener = cleanPower / (power + 1e-9f);

        state.rawGain[k] = juce::jlimit(minGain, 1.0f, (1.0f - amount) + amount * wiener);
    }

    // Suavizado espectral de 5 puntos (anti musical-noise)
    float gainSum = 0.0f;
    for (int k = 0; k < numBins; ++k)
    {
        float g = state.rawGain[k];
        if (k > 1 && k < numBins - 2)
        {
            g = 0.10f * state.rawGain[k - 2] + 0.20f * state.rawGain[k - 1]
              + 0.40f * state.rawGain[k]
              + 0.20f * state.rawGain[k + 1] + 0.10f * state.rawGain[k + 2];
        }

        // En VOCAL: ataque de apertura más rápido para no comerse el inicio de palabras
        const float coeff = (mode == 1 && g > state.smoothGain[k]) ? (timeSmooth * 0.45f) : timeSmooth;
        state.smoothGain[k] = coeff * state.smoothGain[k] + (1.0f - coeff) * g;
        gainSum += state.smoothGain[k];

        state.fftData[2 * k]     *= state.smoothGain[k];
        state.fftData[2 * k + 1] *= state.smoothGain[k];
    }
    state.lastAvgGain = gainSum / static_cast<float>(numBins);

    fft.performRealOnlyInverseTransform(state.fftData.data());
    window.multiplyWithWindowingTable(state.fftData.data(), fftSize);

    constexpr float normFactor = 2.0f / 3.0f;
    for (int i = 0; i < fftSize; ++i)
        state.outputAccum[i] += state.fftData[i] * normFactor;

    std::copy(state.outputAccum.begin(), state.outputAccum.begin() + hopSize, state.outputReady.begin());
    std::copy(state.outputAccum.begin() + hopSize, state.outputAccum.end(), state.outputAccum.begin());
    std::fill(state.outputAccum.end() - hopSize, state.outputAccum.end(), 0.0f);
    std::copy(state.inputBuffer.begin() + hopSize, state.inputBuffer.end(), state.inputBuffer.begin());
}

void DeReverbenjaminV3Processor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int mode = static_cast<int>(apvts.getRawParameterValue("mode")->load());
    const float roomAmount = apvts.getRawParameterValue("roomAmount")->load();
    const float focus = apvts.getRawParameterValue("focus")->load();
    const float decay = apvts.getRawParameterValue("decay")->load();
    const float smooth = apvts.getRawParameterValue("smooth")->load();

    const float tailAmount = apvts.getRawParameterValue("tailAmount")->load();
    const float sensitivity = apvts.getRawParameterValue("sensitivity")->load();
    const float releaseMs = apvts.getRawParameterValue("release")->load();
    const float outGain = juce::Decibels::decibelsToGain(apvts.getRawParameterValue("output")->load());

    const float res1 = apvts.getRawParameterValue("res1")->load();
    const float res2 = apvts.getRawParameterValue("res2")->load();
    const float res3 = apvts.getRawParameterValue("res3")->load();
    const float mix = apvts.getRawParameterValue("mix")->load();

    const int numChannels = std::min(buffer.getNumChannels(), static_cast<int>(channels.size()));
    const int numSamples = buffer.getNumSamples();
    const float sr = static_cast<float>(currentSampleRate);
    constexpr float twoPi = juce::MathConstants<float>::twoPi;

    // Constantes Etapa 2 (De-Tail) adaptadas por instrumento
    const float fastAttack = std::exp(-1.0f / (sr * (mode == 0 ? 0.002f : 0.003f)));
    const float fastRelease = std::exp(-1.0f / (sr * (mode == 1 ? 0.055f : 0.038f)));
    const float slowAttack = std::exp(-1.0f / (sr * (mode == 1 ? 0.035f : 0.045f)));
    const float effectiveRelMs = (mode == 1) ? std::min(releaseMs, 180.0f) : releaseMs;
    const float slowRelease = std::exp(-1.0f / (sr * (effectiveRelMs * 0.001f)));
    const float gainSmoothAtk = std::exp(-1.0f / (sr * (mode == 1 ? 0.004f : 0.012f)));
    const float gainSmoothRel = std::exp(-1.0f / (sr * 0.014f));

    // Constantes Etapa 3: Detector de transientes suavizado
    const float transFastAtk = std::exp(-1.0f / (sr * 0.0020f));
    const float transFastRel = std::exp(-1.0f / (sr * (mode == 0 ? 0.018f : 0.028f)));
    const float transSlowAtk = std::exp(-1.0f / (sr * 0.018f));
    const float transSlowRel = std::exp(-1.0f / (sr * (mode == 0 ? 0.085f : 0.130f)));
    const float transSmoothCoeff = std::exp(-1.0f / (sr * 0.003f));

    // Sub-Thump (DRUMS): oscilador limpio a 62 Hz
    const float subPhaseInc = twoPi * 62.0f / sr;
    const float subDecayCoeff = std::exp(-1.0f / (sr * 0.055f));
    const float dryLowCoeff = std::exp(-twoPi * 115.0f / sr);

    // Coeficientes de bandas según instrumento
    const float lowCutHz   = (mode == 0) ? 120.0f : (mode == 1 ? 260.0f : (mode == 2 ? 200.0f : 220.0f));
    const float midLoHz    = (mode == 1) ? 2200.0f : 1800.0f;
    const float midHiHz    = (mode == 1) ? 4800.0f : 4200.0f;
    const float airCutHz   = (mode == 0) ? 5000.0f : (mode == 1 ? 7500.0f : (mode == 2 ? 4200.0f : 6500.0f));

    const float lowLpCoeff   = std::exp(-twoPi * lowCutHz / sr);
    const float midLoCoeff   = std::exp(-twoPi * midLoHz / sr);
    const float midHiCoeff   = std::exp(-twoPi * midHiHz / sr);
    const float airLpCoeff   = std::exp(-twoPi * airCutHz / sr);
    const float noiseHpCoeff = std::exp(-twoPi * (mode == 0 ? 4500.0f : 7200.0f) / sr);
    const float noiseLpCoeff = std::exp(-twoPi * 13500.0f / sr);

    // Envolvente Opto-Glue y Gate de aire limpio (VOCAL)
    const float optoAtk = std::exp(-1.0f / (sr * 0.015f));
    const float optoRel = std::exp(-1.0f / (sr * 0.140f));
    const float airGateAtk = std::exp(-1.0f / (sr * 0.006f));
    const float airGateRel = std::exp(-1.0f / (sr * 0.060f));

    float spectralGainAvg = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        float dryPeak = 0.0f;
        float wetPeak = 0.0f;
        float dryLowPeak = 0.0f;
        float dryAlignedSamples[2] = { 0.0f, 0.0f };
        float wetSamples[2] = { 0.0f, 0.0f };

        // --- ETAPA 1: Spectral De-Room + Señal Seca Alineada ---
        float frameGainSum = 0.0f;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& state = channels[static_cast<size_t>(ch)];
            const float inSample = buffer.getSample(ch, i);

            const float dryAligned = state.dryDelay[static_cast<size_t>(state.dryWritePos)];
            state.dryDelay[static_cast<size_t>(state.dryWritePos)] = inSample;
            state.dryWritePos = (state.dryWritePos + 1) & (fftSize - 1);

            state.inputBuffer[fftSize - hopSize + state.hopIndex] = inSample;
            const float spectralOut = state.outputReady[state.hopIndex];
            state.hopIndex++;

            if (state.hopIndex == hopSize)
            {
                state.hopIndex = 0;
                processFrame(state, roomAmount, decay, focus, smooth, mode);
            }

            state.dryLowLp = dryLowCoeff * state.dryLowLp + (1.0f - dryLowCoeff) * dryAligned;

            if (ch < 2)
            {
                dryAlignedSamples[ch] = dryAligned;
                wetSamples[ch] = spectralOut;
            }
            dryPeak = std::max(dryPeak, std::abs(dryAligned));
            wetPeak = std::max(wetPeak, std::abs(spectralOut));
            dryLowPeak = std::max(dryLowPeak, std::abs(state.dryLowLp));
            frameGainSum += state.lastAvgGain;
        }
        if (numChannels > 0)
            spectralGainAvg = frameGainSum / static_cast<float>(numChannels);

        // --- ETAPA 2: Dual-Envelope Tail De-Reverb (Específico por instrumento) ---
        fastEnv = (wetPeak > fastEnv)
            ? fastAttack * fastEnv + (1.0f - fastAttack) * wetPeak
            : fastRelease * fastEnv + (1.0f - fastRelease) * wetPeak;

        slowEnv = (wetPeak > slowEnv)
            ? slowAttack * slowEnv + (1.0f - slowAttack) * wetPeak
            : slowRelease * slowEnv + (1.0f - slowRelease) * wetPeak;

        float ratio = fastEnv / (slowEnv + 1e-6f);
        float targetGain = 1.0f;
        if (ratio < sensitivity)
        {
            float reduction = juce::jlimit(0.0f, 1.0f, ratio / sensitivity);
            if (mode == 1)
            {
                // VOCAL: curva suave + piso de seguridad para no hundir palabras suaves
                const float vocalCurve = reduction * std::sqrt(reduction);
                targetGain = std::max(0.24f, (1.0f - tailAmount) + (tailAmount * vocalCurve));
            }
            else
            {
                // DRUMS y otros: curva cúbica intacta de v3
                targetGain = (1.0f - tailAmount) + (tailAmount * reduction * reduction * reduction);
            }
        }
        const float gsCoeff = (targetGain > tailSmoothGain) ? gainSmoothAtk : gainSmoothRel;
        tailSmoothGain = gsCoeff * tailSmoothGain + (1.0f - gsCoeff) * targetGain;

        // --- ETAPA 3: Envolventes de Resíntesis ---
        dryTransFast = (dryPeak > dryTransFast)
            ? transFastAtk * dryTransFast + (1.0f - transFastAtk) * dryPeak
            : transFastRel * dryTransFast + (1.0f - transFastRel) * dryPeak;

        dryTransSlow = (dryPeak > dryTransSlow)
            ? transSlowAtk * dryTransSlow + (1.0f - transSlowAtk) * dryPeak
            : transSlowRel * dryTransSlow + (1.0f - transSlowRel) * dryPeak;

        const float rawTrans = juce::jlimit(0.0f, 1.0f, (dryTransFast - dryTransSlow) / (dryTransSlow + 0.03f));
        transSmooth = transSmoothCoeff * transSmooth + (1.0f - transSmoothCoeff) * rawTrans;

        // Detector de golpe grave para Sub-Thump (DRUMS)
        lowTransFast = (dryLowPeak > lowTransFast)
            ? transFastAtk * lowTransFast + (1.0f - transFastAtk) * dryLowPeak
            : transFastRel * lowTransFast + (1.0f - transFastRel) * dryLowPeak;
        lowTransSlow = (dryLowPeak > lowTransSlow)
            ? transSlowAtk * lowTransSlow + (1.0f - transSlowAtk) * dryLowPeak
            : transSlowRel * lowTransSlow + (1.0f - transSlowRel) * dryLowPeak;

        const float lowHit = std::max(0.0f, lowTransFast - lowTransSlow * 1.15f);
        subEnv = std::max(subEnv * subDecayCoeff, juce::jlimit(0.0f, 0.85f, lowHit * 2.2f));
        subPhase += subPhaseInc;
        if (subPhase >= twoPi) subPhase -= twoPi;
        const float subSample = std::sin(subPhase) * subEnv;

        // Envolventes para VOCAL (Opto-Glue y Gate de Aire)
        const float cleanPeak = wetPeak * tailSmoothGain;
        vocalOptoEnv = (cleanPeak > vocalOptoEnv)
            ? optoAtk * vocalOptoEnv + (1.0f - optoAtk) * cleanPeak
            : optoRel * vocalOptoEnv + (1.0f - optoRel) * cleanPeak;

        airGateEnv = (cleanPeak > airGateEnv)
            ? airGateAtk * airGateEnv + (1.0f - airGateAtk) * cleanPeak
            : airGateRel * airGateEnv + (1.0f - airGateRel) * cleanPeak;
        const float cleanGate = juce::jlimit(0.0f, 1.0f, (airGateEnv - 0.012f) * 14.0f);

        const float spectralLoss = juce::jlimit(0.0f, 1.0f, 1.0f - spectralGainAvg);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            auto& state = channels[static_cast<size_t>(ch)];
            const float dry = (ch < 2) ? dryAlignedSamples[ch] : 0.0f;
            float sig = ((ch < 2) ? wetSamples[ch] : 0.0f) * tailSmoothGain;

            // Bandas de la señal limpia
            state.lowLp   = lowLpCoeff * state.lowLp + (1.0f - lowLpCoeff) * sig;
            state.midLoLp = midLoCoeff * state.midLoLp + (1.0f - midLoCoeff) * sig;
            state.midHiLp = midHiCoeff * state.midHiLp + (1.0f - midHiCoeff) * sig;
            state.airLp   = airLpCoeff * state.airLp + (1.0f - airLpCoeff) * sig;

            const float lowBand  = state.lowLp;
            const float midBand  = state.midHiLp - state.midLoLp;
            const float highBand = sig - state.airLp;

            // Ruido blanco filtrado para resíntesis de aire / bordona (100% seco)
            const float wn = state.nextWhiteNoise();
            state.noiseHpState = noiseHpCoeff * state.noiseHpState + (1.0f - noiseHpCoeff) * wn;
            const float hpNoise = wn - state.noiseHpState;
            state.noiseLpState = noiseLpCoeff * state.noiseLpState + (1.0f - noiseLpCoeff) * hpNoise;
            const float shapedAirNoise = state.noiseLpState;

            if (mode == 0) // === DRUMS: PUNCH / THUMP / SNAP ===
            {
                sig += (dry - sig) * (transSmooth * res1 * 0.78f);

                if (res2 > 0.001f)
                    sig += (subSample * 0.42f + std::tanh(lowBand * 1.8f) * 0.25f) * res2;

                if (res3 > 0.001f)
                {
                    const float snapNoise = shapedAirNoise * transSmooth * cleanGate * 0.14f;
                    const float snapHigh = std::tanh(highBand * 2.0f) * 0.45f;
                    sig += (snapNoise + snapHigh) * res3 * (0.7f + spectralLoss * 0.5f);
                }
            }
            else if (mode == 1) // === VOCAL: BODY / PRESENCE / SILK + AUTO-LEVELER ===
            {
                // Compensación automática de volumen cuando hay mucha supresión (solo mientras se canta)
                const float vocalMakeup = 1.0f + (spectralLoss * 0.34f * cleanGate);
                sig *= vocalMakeup;

                // 1. BODY: efecto de proximidad + armónicos pares suaves estilo tubo
                if (res1 > 0.001f)
                {
                    const float evenHarm = lowBand + 0.28f * lowBand * lowBand;
                    sig += std::tanh(evenHarm * 1.6f) * res1 * 0.48f;
                }

                // 2. PRESENCE: rescate de consonantes + Opto-Glue de estudio
                if (res2 > 0.001f)
                {
                    sig += (dry - sig) * (transSmooth * res2 * 0.46f);
                    sig += std::tanh(midBand * 1.7f) * res2 * 0.35f;
                    const float glue = 1.0f / (1.0f + vocalOptoEnv * res2 * 1.5f);
                    sig *= (glue * (1.0f + res2 * 0.30f));
                }

                // 3. SILK: aire de micrófono de condensador resintetizado (con gate en pausas)
                if (res3 > 0.001f)
                {
                    const float vocalAir = shapedAirNoise * airGateEnv * cleanGate * 0.16f;
                    const float silkSheen = std::tanh(highBand * 1.8f) * 0.40f;
                    sig += (vocalAir + silkSheen) * res3 * (0.65f + spectralLoss * 0.5f);
                }
            }
            else if (mode == 2) // === GTR/BASS: ATTACK / WARMTH / BITE ===
            {
                sig += (dry - sig) * (transSmooth * res1 * 0.72f);

                if (res2 > 0.001f)
                {
                    const float drive = 1.0f + res2 * (1.6f + spectralLoss * 2.0f);
                    const float sat = std::tanh(sig * drive) / std::tanh(drive);
                    sig = sig * (1.0f - res2 * 0.65f) + sat * (res2 * 0.65f);
                }

                if (res3 > 0.001f)
                    sig += std::tanh(midBand * 2.4f) * res3 * 0.45f;
            }
            else // === KEYS: HAMMER / RICHNESS / SHIMMER ===
            {
                sig += (dry - sig) * (transSmooth * res1 * 0.68f);

                if (res2 > 0.001f)
                {
                    const float rich = sig + 0.22f * sig * sig;
                    sig = sig * (1.0f - res2 * 0.45f) + std::tanh(rich * 1.4f) * (res2 * 0.45f);
                }

                if (res3 > 0.001f)
                {
                    const float keyAir = shapedAirNoise * airGateEnv * cleanGate * 0.08f;
                    sig += (std::tanh(highBand * 2.0f) * 0.45f + keyAir) * res3;
                }
            }

            // Bloqueador de DC
            const float dcBlocked = sig - state.dcPrevIn + 0.995f * state.dcPrevOut;
            state.dcPrevIn = sig;
            state.dcPrevOut = dcBlocked;

            // Mezcla paralela en fase perfecta + Salida
            const float finalOut = (dry * (1.0f - mix) + dcBlocked * mix) * outGain;
            buffer.setSample(ch, i, finalOut);
        }
    }

    currentGainReduction.store(juce::jlimit(0.0f, 1.0f, spectralGainAvg * tailSmoothGain));
}

juce::AudioProcessorEditor* DeReverbenjaminV3Processor::createEditor()
{
    return new DeReverbenjaminV3Editor(*this);
}

void DeReverbenjaminV3Processor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, destData);
}

void DeReverbenjaminV3Processor::setStateInformation(const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xmlState(getXmlFromBinary(data, sizeInBytes));
    if (xmlState != nullptr && xmlState->hasTagName(apvts.state.getType()))
        apvts.replaceState(juce::ValueTree::fromXml(*xmlState));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeReverbenjaminV3Processor();
}
