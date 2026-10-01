#pragma once
#include "aeffeditor.h"

class SimpleDSPUiSource {
public:
    virtual ~SimpleDSPUiSource() = default;
    virtual float uiParameter(int index) const = 0;
    virtual void uiSetParameter(int index, float value) = 0;
    virtual void uiBeginEdit(int index) = 0;
    virtual void uiEndEdit(int index) = 0;
    virtual void uiParameterText(int index, char* text, int size) const = 0;
    virtual void uiSetParameterText(int index, const char* text) = 0;
    virtual void uiCopyWave(int count, float* input, float* output) const = 0;
    virtual float uiInputDb() const = 0;
    virtual float uiOutputDb() const = 0;
    virtual float uiInputPeakDb() const = 0;
    virtual float uiOutputPeakDb() const = 0;
    virtual float uiLoudnessGainDb() const = 0;
    virtual float uiLoudnessPeakGainDb() const = 0;
    virtual float uiLimiterGainDb() const = 0;
};

AEffEditor* createSimpleDSPEditor(AudioEffect* effect, SimpleDSPUiSource* source);
