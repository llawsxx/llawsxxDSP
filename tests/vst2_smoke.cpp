#include "aeffectx.h"
#include <windows.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

static VstIntPtr VSTCALLBACK host(AEffect*, VstInt32 opcode, VstInt32, VstIntPtr, void*, float) {
    if (opcode == audioMasterVersion) return 2400;
    if (opcode == audioMasterUpdateDisplay || opcode == audioMasterSizeWindow) return 1;
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // Keep third-party IMEs out of the synthetic editor lifecycle test. Some
    // IMEs are not AddressSanitizer-clean and otherwise obscure plugin errors.
    ImmDisableIME(static_cast<DWORD>(-1));
    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) { std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 3; }
    using Main = AEffect* (*)(audioMasterCallback);
    auto mainEntry = reinterpret_cast<Main>(GetProcAddress(module, "VSTPluginMain"));
    if (!mainEntry) return 4;
    AEffect* effect = mainEntry(host);
    if (!effect || effect->magic != kEffectMagic || effect->numInputs != 2 ||
        effect->numOutputs != 2 || effect->numParams != 29 || !(effect->flags & effFlagsHasEditor)) return 5;
    if (effect->getParameter(effect, 0) >= 0.5f || effect->getParameter(effect, 13) >= 0.5f ||
        effect->getParameter(effect, 18) >= 0.5f || effect->getParameter(effect, 22) >= 0.5f) return 15;
    effect->dispatcher(effect, effOpen, 0, 0, nullptr, 0);
    effect->dispatcher(effect, effSetSampleRate, 0, 0, nullptr, 48000.f);
    effect->dispatcher(effect, effSetBlockSize, 0, 256, nullptr, 0);
    effect->dispatcher(effect, effMainsChanged, 0, 1, nullptr, 0);
    constexpr int frames = 256;
    std::vector<float> inL(frames), inR(frames), outL(frames), outR(frames);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    bool heard = false;
    float bypassError = 0.f;
    double phase = 0;
    for (int block = 0; block < 8; ++block) {
        for (int i = 0; i < frames; ++i) {
            inL[i] = inR[i] = 0.5f * std::sin(static_cast<float>(phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        }
        effect->processReplacing(effect, inputs, outputs, frames);
        for (int i = 0; i < frames; ++i) {
            if (!std::isfinite(outL[i]) || !std::isfinite(outR[i]) || std::fabs(outL[i]) > 1.01f) return 6;
            heard = heard || std::fabs(outL[i]) > 0.01f;
            bypassError = std::max(bypassError, std::fabs(outL[i] - inL[i]));
        }
    }
    if (!heard) return 7;
    if (bypassError > 0.000001f) return 19;

    effect->setParameter(effect, 0, 1.f);
    effect->setParameter(effect, 1, std::log(1000.f / 20.f) / std::log(20000.f / 20.f));
    effect->setParameter(effect, 2, 1.f);
    float eqPeak = 0.f;
    for (int block = 0; block < 40; ++block) {
        for (int i = 0; i < frames; ++i) {
            inL[i] = inR[i] = 0.1f * std::sin(static_cast<float>(phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        }
        effect->processReplacing(effect, inputs, outputs, frames);
        if (block > 30) for (float sample : outL) eqPeak = std::max(eqPeak, std::fabs(sample));
    }
    if (eqPeak < 0.45f) return 20;
    effect->setParameter(effect, 0, 0.f);
    effect->setParameter(effect, 2, 0.5f);

    effect->setParameter(effect, 22, 1.f);
    effect->setParameter(effect, 23, 1.f);
    float limitedPeak = 0.f;
    for (int block = 0; block < 12; ++block) {
        for (int i = 0; i < frames; ++i) {
            inL[i] = inR[i] = 0.9f * std::sin(static_cast<float>(phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        }
        effect->processReplacing(effect, inputs, outputs, frames);
        for (float sample : outL) limitedPeak = std::max(limitedPeak, std::fabs(sample));
    }
    if (limitedPeak > 0.902f || limitedPeak < 0.1f) return 21;
    effect->setParameter(effect, 22, 0.f);

    effect->setParameter(effect, 18, 1.f);
    float normalizedPeak = 0.f;
    for (int block = 0; block < 950; ++block) {
        for (int i = 0; i < frames; ++i) {
            inL[i] = inR[i] = 0.03f * std::sin(static_cast<float>(phase));
            phase += 2.0 * 3.14159265358979323846 * 1000.0 / 48000.0;
        }
        effect->processReplacing(effect, inputs, outputs, frames);
        if (block > 900) for (float sample : outL) normalizedPeak = std::max(normalizedPeak, std::fabs(sample));
    }
    if (normalizedPeak < 0.06f || normalizedPeak > 0.91f) return 22;
    effect->setParameter(effect, 18, 0.f);

    effect->setParameter(effect, 13, 1.f);
    effect->setParameter(effect, 17, 1.f);
    Sleep(250);
    bool reverbHeard = false;
    for (int block = 0; block < 40; ++block) {
        std::fill(inL.begin(), inL.end(), 0.f);
        std::fill(inR.begin(), inR.end(), 0.f);
        if (block == 0) inL[0] = inR[0] = 1.f;
        effect->processReplacing(effect, inputs, outputs, frames);
        for (float sample : outL) reverbHeard = reverbHeard || std::fabs(sample) > 0.00001f;
    }
    if (!reverbHeard) return 23;
    effect->setParameter(effect, 13, 0.f);
    effect->setParameter(effect, 14, 0.5f);
    ERect* rect = nullptr;
    if (!effect->dispatcher(effect, effEditGetRect, 0, 0, &rect, 0) || !rect || rect->right != 920 || rect->bottom != 650) return 8;
    HWND parent = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 750, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent || !effect->dispatcher(effect, effEditOpen, 0, 0, parent, 0)) return 9;
    effect->dispatcher(effect, effEditIdle, 0, 0, nullptr, 0);
    HWND editor = GetWindow(parent, GW_CHILD);
    if (!editor) return 10;
    const float eqBefore = effect->getParameter(effect, 0);
    SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(120, 325));
    SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(120, 325));
    const float eqAfter = effect->getParameter(effect, 0);
    if ((eqBefore >= 0.5f) == (eqAfter >= 0.5f)) return 11;
    const float gainBefore = effect->getParameter(effect, 2);
    SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(390, 437));
    SendMessageW(editor, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(490, 437));
    SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(490, 437));
    if (effect->getParameter(effect, 2) <= gainBefore) return 12;
    SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(250, 365));
    SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(250, 365));
    const float roomBefore = effect->getParameter(effect, 14);
    SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(180, 425));
    SendMessageW(editor, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(350, 425));
    SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(350, 425));
    if (effect->getParameter(effect, 14) <= roomBefore) return 16;
    HWND valueEdit = nullptr;
    SendMessageW(editor, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(360, 410));
    SendMessageW(editor, WM_LBUTTONUP, 0, MAKELPARAM(360, 410));
    valueEdit = GetWindow(editor, GW_CHILD);
    if (!valueEdit) return 17;
    SetWindowTextA(valueEdit, "75");
    SendMessageW(valueEdit, WM_KEYDOWN, VK_RETURN, 0);
    SendMessageW(editor, WM_APP + 37, 1, 0);
    if (effect->getParameter(effect, 14) < 0.74f || effect->getParameter(effect, 14) > 0.76f) return 18;
    effect->dispatcher(effect, effEditClose, 0, 0, nullptr, 0);
    DestroyWindow(parent);
    parent = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 750, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent || !effect->dispatcher(effect, effEditOpen, 0, 0, parent, 0)) return 13;
    DestroyWindow(parent);
    parent = CreateWindowExW(0, L"STATIC", L"host", WS_OVERLAPPEDWINDOW, 0, 0, 1000, 750, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent || !effect->dispatcher(effect, effEditOpen, 0, 0, parent, 0)) return 14;
    effect->dispatcher(effect, effEditClose, 0, 0, nullptr, 0);
    DestroyWindow(parent);
    effect->dispatcher(effect, effMainsChanged, 0, 0, nullptr, 0);
    effect->dispatcher(effect, effClose, 0, 0, nullptr, 0);
    FreeLibrary(module);
    return 0;
}
