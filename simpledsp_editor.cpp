#include "simpledsp_editor.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>

namespace
{
    constexpr int Width = 920, Height = 650, ParamCount = 29, WaveMax = 16384;
    constexpr UINT CommitEditMessage = WM_APP + 37;
    constexpr int SwitchTop = 314, TabTop = 350, ContentTop = 398;
    constexpr int moduleParams[4] = {0, 13, 18, 22};
    constexpr int tabCounts[4] = {12, 4, 3, 6};
    constexpr int tabParams[4][12] = {
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12},
        {14, 15, 16, 17, -1, -1, -1, -1, -1, -1, -1, -1},
        {19, 20, 21, -1, -1, -1, -1, -1, -1, -1, -1, -1},
        {23, 24, 25, 26, 27, 28, -1, -1, -1, -1, -1, -1}};
    const char *moduleNames[4] = {"EQUALIZER", "REVERB", "LOUDNESS", "LIMITER"};
    const char *names[ParamCount] = {
        "EQ On", "EQ1 Freq", "EQ1 Gain", "EQ1 Q", "EQ2 Freq", "EQ2 Gain", "EQ2 Q",
        "EQ3 Freq", "EQ3 Gain", "EQ3 Q", "EQ4 Freq", "EQ4 Gain", "EQ4 Q", "Rev On",
        "Room", "Decay", "Damping", "Mix", "Loud On", "Target", "LRA", "TruePk", "Lim On",
        "Input", "Limit", "Release", "Ceiling", "Lookahead", "Adaptive"};
    float clampf(float x, float a, float b) { return std::max(a, std::min(b, x)); }
    UINT windowDpi(HWND window)
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
        using GetDpiForSystemFn = UINT(WINAPI *)();
        if (window)
            if (auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(user32, "GetDpiForWindow")))
                return std::clamp(getDpiForWindow(window), 96u, 384u);
        if (auto getDpiForSystem = reinterpret_cast<GetDpiForSystemFn>(GetProcAddress(user32, "GetDpiForSystem")))
            return std::clamp(getDpiForSystem(), 96u, 384u);
        HDC screen = GetDC(nullptr);
        UINT dpi = screen ? static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX)) : 96u;
        if (screen)
            ReleaseDC(nullptr, screen);
        return std::clamp(dpi, 96u, 384u);
    }
    int dpiScale(int value, UINT dpi) { return MulDiv(value, static_cast<int>(dpi), 96); }
    void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
    {
        RECT x{l, t, r, b};
        HBRUSH h = CreateSolidBrush(c);
        FillRect(dc, &x, h);
        DeleteObject(h);
    }
    void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int width = 1)
    {
        HPEN p = CreatePen(PS_SOLID, width, c);
        HGDIOBJ old = SelectObject(dc, p);
        MoveToEx(dc, x1, y1, nullptr);
        LineTo(dc, x2, y2);
        SelectObject(dc, old);
        DeleteObject(p);
    }
    void text(HDC dc, int x, int y, COLORREF c, const char *s)
    {
        SetTextColor(dc, c);
        TextOutA(dc, x, y, s, (int)std::strlen(s));
    }

    class Editor final : public AEffEditor
    {
    public:
        Editor(AudioEffect *effect, SimpleDSPUiSource *ui) : AEffEditor(effect), source(ui), dpi(windowDpi(nullptr)) { updateRect(); }
        ~Editor() override { close(); }
        bool getRect(ERect **result) override
        {
            if (!window)
            {
                dpi = windowDpi(nullptr);
                updateRect();
            }
            *result = &editorRect;
            return true;
        }
        bool isOpen() override { return window && IsWindow(window); }
        bool open(void *parent) override
        {
            if (window)
                return true;
            const VstInt16 previousWidth = editorRect.right;
            const VstInt16 previousHeight = editorRect.bottom;
            dpi = windowDpi(static_cast<HWND>(parent));
            updateRect();
            window = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_NOTIFY,
                                     0, 0, editorRect.right, editorRect.bottom, (HWND)parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window)
                return false;
            EnableWindow(window, TRUE);
            SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)this);
            original = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)windowProc);
            systemWindow = window;
            if (editorRect.right != previousWidth || editorRect.bottom != previousHeight)
                static_cast<AudioEffectX *>(effect)->sizeWindow(editorRect.right, editorRect.bottom);
            SetTimer(window, 1, 33, nullptr);
            return true;
        }
        void close() override
        {
            if (window)
            {
                KillTimer(window, 1);
                DestroyWindow(window);
                window = nullptr;
            }
            systemWindow = nullptr;
        }
        void idle() override
        {
            if (window)
                InvalidateRect(window, nullptr, FALSE);
        }

    private:
        static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            Editor *self = (Editor *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (!self)
                return DefWindowProcW(hwnd, message, wParam, lParam);
            switch (message)
            {
            case WM_TIMER:
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
                self->paint();
                return 0;
            case WM_LBUTTONDOWN:
            {
                POINT point = self->logicalPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                self->mouseDown(point.x, point.y);
                return 0;
            }
            case WM_MOUSEMOVE:
                if (wParam & MK_LBUTTON)
                    self->mouseMove(self->logicalPoint(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)).x);
                return 0;
            case WM_LBUTTONUP:
                self->mouseUp();
                return 0;
            case CommitEditMessage:
                self->finishEdit(wParam != 0);
                return 0;
            case WM_DPICHANGED:
                self->changeDpi(HIWORD(wParam));
                return 0;
            case WM_NCDESTROY:
            {
                WNDPROC proc = self->original;
                KillTimer(hwnd, 1);
                if (GetCapture() == hwnd)
                    ReleaseCapture();
                self->finishEdit(false);
                self->dragging = -1;
                self->window = nullptr;
                self->systemWindow = nullptr;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return CallWindowProcW(proc, hwnd, message, wParam, lParam);
            }
            default:
                return CallWindowProcW(self->original, hwnd, message, wParam, lParam);
            }
        }

        void drawWave(HDC dc, int top, const char *label, const float *values, COLORREF color)
        {
            const int left = 20, right = 670, bottom = top + 105, width = right - left - 2;
            fill(dc, left, top, right, bottom, RGB(11, 14, 19));
            line(dc, left, (top + bottom) / 2, right, (top + bottom) / 2, RGB(55, 60, 68));
            text(dc, left + 7, top + 5, RGB(180, 188, 200), label);
            for (int x = 0; x < width; x++)
            {
                int first = x * density / width, last = std::max(first + 1, (x + 1) * density / width);
                float lo = values[first], hi = values[first];
                for (int i = first + 1; i < last; i++)
                {
                    lo = std::min(lo, values[i]);
                    hi = std::max(hi, values[i]);
                }
                int mid = (top + bottom) / 2;
                int y1 = mid - (int)(clampf(hi, -1.f, 1.f) * 43.f), y2 = mid - (int)(clampf(lo, -1.f, 1.f) * 43.f);
                line(dc, left + 1 + x, y1, left + 1 + x, y2, color);
            }
        }

        void drawMeter(HDC dc, int x, const char *label, float now, float peak, COLORREF color)
        {
            text(dc, x, 39, RGB(210, 216, 224), label);
            const int top = 65, bottom = 244, width = 58;
            fill(dc, x, top, x + width, bottom, RGB(9, 12, 16));
            float level = clampf((now + 60.f) / 60.f, 0.f, 1.f);
            fill(dc, x + 5, bottom - (int)((bottom - top) * level), x + width - 5, bottom - 4, color);
            float held = clampf((peak + 60.f) / 60.f, 0.f, 1.f);
            int peakY = bottom - (int)((bottom - top) * held);
            line(dc, x + 2, peakY, x + width - 2, peakY, RGB(255, 218, 92), 2);
            for (int db = -60; db <= 0; db += 12)
            {
                int y = bottom - (int)((bottom - top) * (db + 60.f) / 60.f);
                line(dc, x + width + 2, y, x + width + 7, y, RGB(105, 111, 121));
            }
            char current[32], heldText[32];
            std::snprintf(current, sizeof(current), "Now %6.1f dB", now);
            std::snprintf(heldText, sizeof(heldText), "1.5s %5.1f dB", peak);
            text(dc, x - 5, 251, RGB(220, 225, 232), current);
            text(dc, x - 5, 269, RGB(255, 218, 92), heldText);
        }

        RECT moduleRect(int module) const
        {
            int x = 104 + module * 198;
            return RECT{x, SwitchTop, x + 178, SwitchTop + 27};
        }
        RECT tabRect(int tab) const
        {
            int x = 20 + tab * 220;
            return RECT{x, TabTop, x + 210, TabTop + 34};
        }
        RECT controlRect(int slot) const
        {
            if (selectedTab == 0)
            {
                int band = slot / 3, column = slot % 3;
                int x = 20 + column * 293, y = ContentTop + band * 61;
                return RECT{x, y, x + 280, y + 54};
            }
            int column = slot % 2, row = slot / 2;
            int x = 60 + column * 420, y = ContentTop + row * 70;
            return RECT{x, y, x + 360, y + 58};
        }
        RECT inputRect(int slot) const
        {
            RECT r = controlRect(slot);
            return RECT{r.right - 82, r.top + 5, r.right - 7, r.top + 27};
        }
        int sliderStart(const RECT &r) const { return r.left + 10; }
        int sliderY(const RECT &r) const { return r.top + 39; }

        void drawModules(HDC dc)
        {
            text(dc, 20, 321, RGB(176, 184, 195), "DSP:");
            for (int i = 0; i < 4; i++)
            {
                RECT r = moduleRect(i);
                bool on = source->uiParameter(moduleParams[i]) >= .5f;
                fill(dc, r.left, r.top, r.right, r.bottom, on ? RGB(39, 145, 94) : RGB(52, 58, 68));
                text(dc, r.left + 12, r.top + 6, on ? RGB(247, 250, 252) : RGB(170, 178, 189), moduleNames[i]);
                text(dc, r.right - 32, r.top + 6, on ? RGB(212, 255, 231) : RGB(145, 152, 163), on ? "ON" : "OFF");
            }
        }

        void drawTabs(HDC dc)
        {
            for (int i = 0; i < 4; i++)
            {
                RECT r = tabRect(i);
                bool selected = i == selectedTab;
                fill(dc, r.left, r.top, r.right, r.bottom, selected ? RGB(42, 105, 163) : RGB(39, 44, 52));
                if (selected)
                    fill(dc, r.left, r.bottom - 3, r.right, r.bottom, RGB(77, 180, 245));
                text(dc, r.left + 14, r.top + 9, selected ? RGB(248, 250, 253) : RGB(159, 168, 180), moduleNames[i]);
            }
        }

        void drawControls(HDC dc)
        {
            for (int slot = 0; slot < tabCounts[selectedTab]; slot++)
            {
                int parameter = tabParams[selectedTab][slot];
                RECT r = controlRect(slot);
                fill(dc, r.left, r.top, r.right, r.bottom, RGB(28, 32, 40));
                text(dc, r.left + 10, r.top + 8, RGB(198, 205, 215), names[parameter]);
                float value = source->uiParameter(parameter);
                if (parameter == 28)
                {
                    fill(dc, r.left + 92, r.top + 5, r.left + 150, r.top + 29, value >= .5f ? RGB(43, 142, 95) : RGB(58, 63, 72));
                    text(dc, r.left + 109, r.top + 9, RGB(245, 247, 250), value >= .5f ? "ON" : "OFF");
                }
                else
                {
                    RECT input = inputRect(slot);
                    int start = sliderStart(r), end = input.left - 9, y = sliderY(r), knob = start + (int)((end - start) * value);
                    line(dc, start, y, end, y, RGB(64, 70, 80), 5);
                    line(dc, start, y, knob, y, RGB(64, 160, 225), 5);
                    fill(dc, knob - 4, y - 7, knob + 5, y + 8, RGB(225, 232, 240));
                    char display[48];
                    source->uiParameterText(parameter, display, sizeof(display));
                    fill(dc, input.left, input.top, input.right, input.bottom, RGB(12, 15, 20));
                    line(dc, input.left, input.top, input.right, input.top, RGB(76, 84, 96));
                    line(dc, input.left, input.bottom, input.right, input.bottom, RGB(76, 84, 96));
                    text(dc, input.left + 4, input.top + 4, RGB(218, 225, 234), display);
                }
            }
        }

        void paint()
        {
            PAINTSTRUCT ps{};
            HDC target = BeginPaint(window, &ps), dc = CreateCompatibleDC(target);
            RECT client{};
            GetClientRect(window, &client);
            int pixelWidth = std::max(1L, client.right), pixelHeight = std::max(1L, client.bottom);
            HBITMAP bitmap = CreateCompatibleBitmap(target, pixelWidth, pixelHeight);
            HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
            SetMapMode(dc, MM_ANISOTROPIC);
            SetWindowExtEx(dc, Width, Height, nullptr);
            SetViewportExtEx(dc, pixelWidth, pixelHeight, nullptr);
            HFONT font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                     DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            HGDIOBJ oldFont = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc, TRANSPARENT);
            fill(dc, 0, 0, Width, Height, RGB(20, 23, 29));
            text(dc, 20, 12, RGB(238, 242, 248), "llawsxxDSP   EQ > Reverb > Loudness > Limiter");
            std::array<float, WaveMax> input{}, output{};
            source->uiCopyWave(density, input.data(), output.data());
            drawWave(dc, 38, "INPUT WAVEFORM", input.data(), RGB(67, 220, 143));
            drawWave(dc, 154, "OUTPUT WAVEFORM", output.data(), RGB(69, 170, 245));
            text(dc, 20, 279, RGB(175, 182, 194), "Wave samples:");
            int values[5] = {1024, 2048, 4096, 8192, 16384};
            for (int i = 0; i < 5; i++)
            {
                int x = 112 + i * 76;
                fill(dc, x, 274, x + 68, 298, density == values[i] ? RGB(44, 116, 183) : RGB(47, 52, 61));
                char s[16];
                std::snprintf(s, sizeof(s), "%d", values[i]);
                text(dc, x + (values[i] >= 10000 ? 9 : 15), 279, RGB(240, 244, 249), s);
            }
            drawMeter(dc, 700, "INPUT", source->uiInputDb(), source->uiInputPeakDb(), RGB(59, 201, 124));
            drawMeter(dc, 810, "OUTPUT", source->uiOutputDb(), source->uiOutputPeakDb(), RGB(51, 150, 236));
            line(dc, 20, 307, 900, 307, RGB(58, 63, 72));
            drawModules(dc);
            drawTabs(dc);
            drawControls(dc);
            SetMapMode(dc, MM_TEXT);
            BitBlt(target, 0, 0, pixelWidth, pixelHeight, dc, 0, 0, SRCCOPY);
            SelectObject(dc, oldFont);
            SelectObject(dc, oldBitmap);
            if (font)
                DeleteObject(font);
            DeleteObject(bitmap);
            DeleteDC(dc);
            EndPaint(window, &ps);
        }

        static bool inside(const RECT &r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
        POINT logicalPoint(int x, int y) const
        {
            RECT client{};
            GetClientRect(window, &client);
            return POINT{MulDiv(x, Width, std::max(1L, client.right)), MulDiv(y, Height, std::max(1L, client.bottom))};
        }
        RECT physicalRect(const RECT &logical) const
        {
            RECT client{};
            GetClientRect(window, &client);
            int width = std::max(1L, client.right), height = std::max(1L, client.bottom);
            return RECT{MulDiv(logical.left, width, Width), MulDiv(logical.top, height, Height),
                        MulDiv(logical.right, width, Width), MulDiv(logical.bottom, height, Height)};
        }
        void updateRect()
        {
            editorRect = ERect{0, 0, static_cast<VstInt16>(dpiScale(Height, dpi)), static_cast<VstInt16>(dpiScale(Width, dpi))};
        }
        void changeDpi(UINT newDpi)
        {
            newDpi = std::clamp(newDpi, 96u, 384u);
            if (newDpi == dpi)
                return;
            finishEdit(true);
            dpi = newDpi;
            updateRect();
            static_cast<AudioEffectX *>(effect)->sizeWindow(editorRect.right, editorRect.bottom);
            if (window)
                SetWindowPos(window, nullptr, 0, 0, editorRect.right, editorRect.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        int controlAt(int x, int y) const
        {
            for (int slot = 0; slot < tabCounts[selectedTab]; slot++)
                if (inside(controlRect(slot), x, y))
                    return slot;
            return -1;
        }
        void setFromX(int slot, int x)
        {
            int parameter = tabParams[selectedTab][slot];
            RECT r = controlRect(slot), input = inputRect(slot);
            int start = sliderStart(r), end = input.left - 9;
            source->uiSetParameter(parameter, clampf((float)(x - start) / (end - start), 0.f, 1.f));
            InvalidateRect(window, nullptr, FALSE);
        }
        static LRESULT CALLBACK editProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            Editor *self = (Editor *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (!self)
                return DefWindowProcW(hwnd, message, wParam, lParam);
            if (message == WM_KEYDOWN && wParam == VK_RETURN)
            {
                PostMessageW(self->window, CommitEditMessage, 1, 0);
                return 0;
            }
            if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
            {
                PostMessageW(self->window, CommitEditMessage, 0, 0);
                return 0;
            }
            if (message == WM_KILLFOCUS)
                PostMessageW(self->window, CommitEditMessage, 1, 0);
            if (message == WM_NCDESTROY)
            {
                WNDPROC proc = self->originalEdit;
                self->edit = nullptr;
                self->originalEdit = nullptr;
                self->editingParameter = -1;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                return CallWindowProcW(proc, hwnd, message, wParam, lParam);
            }
            return CallWindowProcW(self->originalEdit, hwnd, message, wParam, lParam);
        }
        void beginTextEdit(int slot)
        {
            finishEdit(true);
            editingParameter = tabParams[selectedTab][slot];
            RECT r = physicalRect(inputRect(slot));
            char value[48];
            source->uiParameterText(editingParameter, value, sizeof(value));
            edit = CreateWindowExA(0, "EDIT", value, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                   r.left, r.top, r.right - r.left, r.bottom - r.top, window, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!edit)
            {
                editingParameter = -1;
                return;
            }
            SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)this);
            originalEdit = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)editProc);
            editFont = CreateFontA(-std::max(11, dpiScale(12, dpi)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            SendMessageW(edit, WM_SETFONT, (WPARAM)(editFont ? editFont : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
        }
        void finishEdit(bool commit)
        {
            if (!edit)
                return;
            if (commit && editingParameter >= 0)
            {
                char value[64]{};
                GetWindowTextA(edit, value, sizeof(value));
                source->uiBeginEdit(editingParameter);
                source->uiSetParameterText(editingParameter, value);
                source->uiEndEdit(editingParameter);
            }
            SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)originalEdit);
            SetWindowLongPtrW(edit, GWLP_USERDATA, 0);
            DestroyWindow(edit);
            if (editFont)
                DeleteObject(editFont);
            editFont = nullptr;
            edit = nullptr;
            originalEdit = nullptr;
            editingParameter = -1;
            if (window)
                InvalidateRect(window, nullptr, FALSE);
        }
        void mouseDown(int x, int y)
        {
            if (y >= 274 && y <= 298)
            {
                int values[5] = {1024, 2048, 4096, 8192, 16384};
                for (int i = 0; i < 5; i++)
                    if (x >= 112 + i * 76 && x <= 180 + i * 76)
                    {
                        density = values[i];
                        InvalidateRect(window, nullptr, FALSE);
                        return;
                    }
            }
            for (int i = 0; i < 4; i++)
                if (inside(moduleRect(i), x, y))
                {
                    int p = moduleParams[i];
                    source->uiBeginEdit(p);
                    source->uiSetParameter(p, source->uiParameter(p) >= .5f ? 0.f : 1.f);
                    source->uiEndEdit(p);
                    InvalidateRect(window, nullptr, FALSE);
                    return;
                }
            for (int i = 0; i < 4; i++)
                if (inside(tabRect(i), x, y))
                {
                    selectedTab = i;
                    dragging = -1;
                    InvalidateRect(window, nullptr, FALSE);
                    return;
                }
            int slot = controlAt(x, y);
            if (slot < 0)
                return;
            int parameter = tabParams[selectedTab][slot];
            if (parameter != 28 && inside(inputRect(slot), x, y))
            {
                beginTextEdit(slot);
                return;
            }
            source->uiBeginEdit(parameter);
            if (parameter == 28)
            {
                source->uiSetParameter(parameter, source->uiParameter(parameter) >= .5f ? 0.f : 1.f);
                source->uiEndEdit(parameter);
                InvalidateRect(window, nullptr, FALSE);
            }
            else
            {
                dragging = slot;
                SetCapture(window);
                setFromX(slot, x);
            }
        }
        void mouseMove(int x)
        {
            if (dragging >= 0)
                setFromX(dragging, x);
        }
        void mouseUp()
        {
            if (dragging >= 0)
            {
                source->uiEndEdit(tabParams[selectedTab][dragging]);
                dragging = -1;
                ReleaseCapture();
            }
        }

        SimpleDSPUiSource *source;
        HWND window = nullptr, edit = nullptr;
        HFONT editFont = nullptr;
        WNDPROC original = nullptr, originalEdit = nullptr;
        ERect editorRect{};
        UINT dpi = 96;
        int density = 4096, selectedTab = 0, dragging = -1, editingParameter = -1;
    };
}

AEffEditor *createSimpleDSPEditor(AudioEffect *effect, SimpleDSPUiSource *source) { return new Editor(effect, source); }
#else
AEffEditor *createSimpleDSPEditor(AudioEffect *, SimpleDSPUiSource *) { return nullptr; }
#endif
