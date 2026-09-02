//Vibecoded with Gemini 3.7 Flash High, with Pro 6-month subscription. Yes, I asked Gemini to write this comment in the code as well, I'm code illiterate. I still have to control its code quality though.
#include "vmsys.h"
#include "vmio.h"
#include "vmgraph.h"
#include "vmchset.h"
#include "vmstdlib.h"
#include "vmres.h"
#include "vm4res.h"
#include "vmtimer.h"
#include "vmmm.h"
#include "ExplosionSfx.h"
#include "CalculatorEngine.h"
#include "CalcIcon.h"
#include <stdio.h>
#include <string.h>

#define KScreenWidth  240
#define KScreenHeight 320

#define KGridCols 5
#define KGridRows 6

#define KColor_RGB(r, g, b) ((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3))

enum TAppViewMode
{
    EViewNormal,
    EViewHistory
};

struct TGridBtn
{
    const VMWCHAR* iLabel;
    const VMWCHAR* iAction;
    VMUINT16 iBgColor;
    VMUINT16 iTextColor;
    bool iIsTemplateN;
};

// Global App State
static VMINT g_layer = -1;
static CalculatorEngine g_engine;
static TAppViewMode g_viewMode = EViewNormal;

// Default on launch to CE button (Row 0, Col 4)
static int g_selectedCol = 4;
static int g_selectedRow = 0;

// Individual continuous button glow state (0.0 = resting, 1.0 = fully lit)
static float g_btnGlow[KGridRows][KGridCols] = {
    {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, 0.0f, 0.0f, 0.0f}
};
static VMINT g_fadeAnimTimer = -1;

#define KMaxHistoryGlow 32
static int g_historyFocus = 0;
static int g_historyTop = 0;
static float g_historyGlow[KMaxHistoryGlow] = {0};
static VMINT g_historyAnimTimer = -1;

static int g_pressedCol = -1;
static int g_pressedRow = -1;
static VMINT g_pressTimer = -1;

enum TSlideDirection
{
    ESlideNone,
    ESlideOpening, // Normal -> History (History slides UP from bottom as overlay: +320 -> 0)
    ESlideClosing  // History -> Normal (History slides DOWN to bottom: 0 -> +320)
};

static const int KSlideTotalFrames = 10; // 10 frames * ~16.7ms = 166.7ms (1/6s)
static TSlideDirection g_slideState = ESlideNone;
static int g_slideFrame = 0;
static int g_slideOffsetY = 0; // 0 .. 320 (History Y offset)
static VMINT g_slideTimer = -1;

// Directional Key Hold and Diagonal Navigation
static const int KKeyHoldInitialDelayMs = 500; // Initial delay before repeating (500ms)
static const int KKeyHoldRepeatIntervalMs = 166; // Continuous step interval (166ms)
static bool g_keyUpHeld = false;
static bool g_keyDownHeld = false;
static bool g_keyLeftHeld = false;
static bool g_keyRightHeld = false;
static VMINT g_keyHoldTimer = -1;
static bool g_keyHoldInRepeat = false;

static bool g_isExitingGag = false;
static VMINT g_gagExitTimer = -1;

static VMWCHAR g_expression[KMaxExprLen] = {0};
static VMWCHAR g_result[KMaxResultLen] = {0};
static bool g_resultCalculated = false;
static bool g_hasTemplateN = false;

static TGridBtn g_buttons[KGridRows][KGridCols];

// Forward declarations
static void RenderScreen(void);
static void ExecuteButton(int col, int row);
static void CalculateResult(void);
static void ClearEntry(void);
static void DeleteChar(void);
static void AppendString(const VMWCHAR* str);
static void AppendChar(VMWCHAR ch);

static VMUINT16 BlendRGB565(VMUINT16 c1, VMUINT16 c2, int t)
{
    if (t <= 0) return c1;
    if (t >= 256) return c2;

    int r1 = (c1 >> 11) & 0x1F;
    int g1 = (c1 >> 5) & 0x3F;
    int b1 = c1 & 0x1F;

    int r2 = (c2 >> 11) & 0x1F;
    int g2 = (c2 >> 5) & 0x3F;
    int b2 = c2 & 0x1F;

    int r = r1 + (((r2 - r1) * t) >> 8);
    int g = g1 + (((g2 - g1) * t) >> 8);
    int b = b1 + (((b2 - b1) * t) >> 8);

    return (VMUINT16)((r << 11) | (g << 5) | b);
}

static void OnFocusAnimTick(VMINT tid)
{
    bool active = false;

    for (int r = 0; r < KGridRows; r++)
    {
        for (int c = 0; c < KGridCols; c++)
        {
            if (r == g_selectedRow && c == g_selectedCol)
            {
                // Fade In: 1/6s (+0.15f per 25ms tick)
                if (g_btnGlow[r][c] < 1.0f)
                {
                    g_btnGlow[r][c] += 0.15f;
                    if (g_btnGlow[r][c] > 1.0f) g_btnGlow[r][c] = 1.0f;
                    active = true;
                }
            }
            else
            {
                // Fade Out: 1/6s (-0.15f per 25ms tick)
                if (g_btnGlow[r][c] > 0.0f)
                {
                    g_btnGlow[r][c] -= 0.15f;
                    if (g_btnGlow[r][c] < 0.0f) g_btnGlow[r][c] = 0.0f;
                    active = true;
                }
            }
        }
    }

    if (!active)
    {
        if (g_fadeAnimTimer != -1)
        {
            vm_delete_timer(g_fadeAnimTimer);
            g_fadeAnimTimer = -1;
        }
    }
    RenderScreen();
}

static void StartGridFocusTransition(int newCol, int newRow)
{
    if (g_selectedCol == newCol && g_selectedRow == newRow) return;
    g_selectedCol = newCol;
    g_selectedRow = newRow;

    if (g_fadeAnimTimer == -1)
    {
        g_fadeAnimTimer = vm_create_timer(25, OnFocusAnimTick);
    }
    RenderScreen();
}

static void OnHistoryAnimTick(VMINT tid)
{
    bool active = false;
    int total = g_engine.HistoryCount();
    if (total > KMaxHistoryGlow) total = KMaxHistoryGlow;

    for (int i = 0; i < total; i++)
    {
        if (i == g_historyFocus)
        {
            // Fade in: 1/6s (+0.15f per 25ms tick)
            if (g_historyGlow[i] < 1.0f)
            {
                g_historyGlow[i] += 0.15f;
                if (g_historyGlow[i] > 1.0f) g_historyGlow[i] = 1.0f;
                active = true;
            }
        }
        else
        {
            // Fade out: 1/6s (-0.15f per 25ms tick)
            if (g_historyGlow[i] > 0.0f)
            {
                g_historyGlow[i] -= 0.15f;
                if (g_historyGlow[i] < 0.0f) g_historyGlow[i] = 0.0f;
                active = true;
            }
        }
    }

    if (!active)
    {
        if (g_historyAnimTimer != -1)
        {
            vm_delete_timer(g_historyAnimTimer);
            g_historyAnimTimer = -1;
        }
    }
    RenderScreen();
}

static void StartHistoryFocusTransition(int newFocus, int newTop)
{
    g_historyFocus = newFocus;
    g_historyTop = newTop;

    if (g_historyAnimTimer == -1)
    {
        g_historyAnimTimer = vm_create_timer(25, OnHistoryAnimTick);
    }
    RenderScreen();
}

static void ResetKeyHoldState(void)
{
    g_keyUpHeld = false;
    g_keyDownHeld = false;
    g_keyLeftHeld = false;
    g_keyRightHeld = false;
    if (g_keyHoldTimer != -1)
    {
        vm_delete_timer(g_keyHoldTimer);
        g_keyHoldTimer = -1;
    }
    g_keyHoldInRepeat = false;
}

static void GetNetDirection(int& dx, int& dy)
{
    // Horizontal axis:
    // If both Right and Left are held -> opposite buttons cancel each other (dx = 0)
    // If only Right is held -> dx = 1
    // If only Left is held -> dx = -1
    if (g_keyRightHeld && !g_keyLeftHeld) dx = 1;
    else if (g_keyLeftHeld && !g_keyRightHeld) dx = -1;
    else dx = 0;

    // Vertical axis:
    // If both Down and Up are held -> opposite buttons cancel each other (dy = 0)
    // If only Down is held -> dy = 1
    // If only Up is held -> dy = -1
    if (g_keyDownHeld && !g_keyUpHeld) dy = 1;
    else if (g_keyUpHeld && !g_keyDownHeld) dy = -1;
    else dy = 0;
}

static void StepDirectionalNavigation(int dx, int dy)
{
    if (dx == 0 && dy == 0) return;
    if (g_isExitingGag || g_slideState != ESlideNone) return;

    if (g_viewMode == EViewHistory)
    {
        int total = g_engine.HistoryCount();
        if (total > 0 && dy != 0)
        {
            int newFocus = g_historyFocus + dy;
            if (newFocus < 0)
            {
                newFocus = total - 1;
            }
            else if (newFocus >= total)
            {
                newFocus = 0;
            }

            int newTop = g_historyTop;
            if (newFocus < newTop)
            {
                newTop = newFocus;
            }
            else if (newFocus >= newTop + 5)
            {
                newTop = newFocus - 5 + 1;
            }
            StartHistoryFocusTransition(newFocus, newTop);
        }
    }
    else
    {
        int newCol = (g_selectedCol + dx + KGridCols) % KGridCols;
        int newRow = (g_selectedRow + dy + KGridRows) % KGridRows;
        StartGridFocusTransition(newCol, newRow);
    }
}

static void OnKeyHoldTimerTick(VMINT tid)
{
    if (g_isExitingGag || g_slideState != ESlideNone) return;

    int dx = 0, dy = 0;
    GetNetDirection(dx, dy);

    if (!g_keyUpHeld && !g_keyDownHeld && !g_keyLeftHeld && !g_keyRightHeld)
    {
        if (g_keyHoldTimer != -1)
        {
            vm_delete_timer(g_keyHoldTimer);
            g_keyHoldTimer = -1;
        }
        g_keyHoldInRepeat = false;
        return;
    }

    if (dx != 0 || dy != 0)
    {
        StepDirectionalNavigation(dx, dy);
    }

    if (!g_keyHoldInRepeat)
    {
        g_keyHoldInRepeat = true;
        if (g_keyHoldTimer != -1)
        {
            vm_delete_timer(g_keyHoldTimer);
        }
        g_keyHoldTimer = vm_create_timer(KKeyHoldRepeatIntervalMs, OnKeyHoldTimerTick);
    }
}

static void OnPressTimerExpired(VMINT tid)
{
    g_pressedCol = -1;
    g_pressedRow = -1;
    if (g_pressTimer != -1)
    {
        vm_delete_timer(g_pressTimer);
        g_pressTimer = -1;
    }
    RenderScreen();
}

static void TriggerButtonFade(int col, int row)
{
    g_pressedCol = col;
    g_pressedRow = row;
    if (g_pressTimer != -1)
    {
        vm_delete_timer(g_pressTimer);
        g_pressTimer = -1;
    }
    g_pressTimer = vm_create_timer(100, OnPressTimerExpired);
}

static void OnExplosionExitTimer(VMINT tid)
{
    if (g_gagExitTimer != -1)
    {
        vm_delete_timer(g_gagExitTimer);
        g_gagExitTimer = -1;
    }
    vm_audio_stop_all();
    vm_exit_app();
}

static void OnSlideTimer(VMINT tid)
{
    if (g_slideState == ESlideOpening)
    {
        g_slideFrame++;
        if (g_slideFrame >= KSlideTotalFrames)
        {
            g_slideFrame = KSlideTotalFrames;
            g_slideOffsetY = 0;
            g_slideState = ESlideNone;
            if (g_slideTimer != -1)
            {
                vm_delete_timer(g_slideTimer);
                g_slideTimer = -1;
            }
        }
        else
        {
            // Ease-Out rate 4 (Quartic): starts fast, decelerates to stop
            float rem = 1.0f - (float)g_slideFrame / (float)KSlideTotalFrames;
            float rem4 = rem * rem * rem * rem;
            g_slideOffsetY = (int)(KScreenHeight * rem4 + 0.5f);
        }
    }
    else if (g_slideState == ESlideClosing)
    {
        g_slideFrame++;
        if (g_slideFrame >= KSlideTotalFrames)
        {
            g_slideFrame = KSlideTotalFrames;
            g_slideOffsetY = 0;
            g_slideState = ESlideNone;
            g_viewMode = EViewNormal;
            if (g_slideTimer != -1)
            {
                vm_delete_timer(g_slideTimer);
                g_slideTimer = -1;
            }
        }
        else
        {
            // Ease-Out rate 4 (Quartic): Y = H * (1 - (1 - t)^4)
            float rem = 1.0f - (float)g_slideFrame / (float)KSlideTotalFrames;
            float rem4 = rem * rem * rem * rem;
            float progress = 1.0f - rem4;
            g_slideOffsetY = (int)(KScreenHeight * progress + 0.5f);
        }
    }
    RenderScreen();
}

static void OpenHistoryWithAnimation(void)
{
    if (g_slideState != ESlideNone) return;
    ResetKeyHoldState();
    int total = g_engine.HistoryCount();
    if (total > 0)
    {
        g_historyFocus = total - 1;
        g_historyTop = (total > 5) ? (total - 5) : 0;
        for (int i = 0; i < KMaxHistoryGlow; i++)
        {
            g_historyGlow[i] = (i == g_historyFocus) ? 1.0f : 0.0f;
        }
    }
    else
    {
        g_historyFocus = 0;
        g_historyTop = 0;
        for (int i = 0; i < KMaxHistoryGlow; i++) g_historyGlow[i] = 0.0f;
    }
    g_viewMode = EViewHistory;
    g_slideState = ESlideOpening;
    g_slideFrame = 0;
    g_slideOffsetY = KScreenHeight; // Start from bottom (+320)
    if (g_slideTimer == -1)
    {
        g_slideTimer = vm_create_timer(17, OnSlideTimer);
    }
    RenderScreen();
}

static void CloseHistoryWithAnimation(void)
{
    if (g_slideState != ESlideNone) return;
    ResetKeyHoldState();
    g_slideState = ESlideClosing;
    g_slideFrame = 0;
    g_slideOffsetY = 0; // Start at top (0)
    if (g_slideTimer == -1)
    {
        g_slideTimer = vm_create_timer(17, OnSlideTimer);
    }
    RenderScreen();
}

// Helper functions for UCS-2 strings
static int w_strlen(const VMWCHAR* s)
{
    int len = 0;
    while (s && s[len] != 0) len++;
    return len;
}

static void w_strcpy(VMWCHAR* dest, const VMWCHAR* src, int maxLen = KMaxExprLen)
{
    if (!dest) return;
    int i = 0;
    if (src)
    {
        while (src[i] != 0 && i < maxLen - 1)
        {
            dest[i] = src[i];
            i++;
        }
    }
    dest[i] = 0;
}

static void FormatDoubleResult(double val, VMWCHAR* outBuf, int maxLen)
{
    char buf[64];
    // Check if integer
    if (val == (double)(long long)val && val > -1e14 && val < 1e14)
    {
        sprintf(buf, "%lld", (long long)val);
    }
    else
    {
        sprintf(buf, "%.10g", val);
    }

    int i = 0;
    while (buf[i] != 0 && i < maxLen - 1)
    {
        outBuf[i] = (VMWCHAR)buf[i];
        i++;
    }
    outBuf[i] = 0;
}

static void InitButtons(void)
{
    // Windows 7 Aero Theme Colors
    VMUINT16 cOpBg    = KColor_RGB(232, 240, 250);
    VMUINT16 cOpText  = KColor_RGB(20, 50, 95);

    VMUINT16 cFuncBg   = KColor_RGB(226, 236, 248);
    VMUINT16 cFuncText = KColor_RGB(25, 60, 110);

    VMUINT16 cClearBg   = KColor_RGB(252, 234, 236);
    VMUINT16 cClearText = KColor_RGB(165, 35, 45);

    VMUINT16 cConstBg   = KColor_RGB(244, 248, 253);
    VMUINT16 cConstText = KColor_RGB(20, 30, 45);

    VMUINT16 cEqBg   = KColor_RGB(192, 228, 255);
    VMUINT16 cEqText = KColor_RGB(10, 45, 100);

    // Row 0: √, ^, log, DEG/RAD, CE
    g_buttons[0][0] = { (const VMWCHAR*)u"√",    (const VMWCHAR*)u"√",    cFuncBg,  cFuncText,  false };
    g_buttons[0][1] = { (const VMWCHAR*)u"^",    (const VMWCHAR*)u"^",    cOpBg,    cOpText,    false };
    g_buttons[0][2] = { (const VMWCHAR*)u"log",  (const VMWCHAR*)u"log(", cFuncBg,  cFuncText,  false };
    g_buttons[0][3] = { (const VMWCHAR*)u"DEG",  (const VMWCHAR*)u"DEG",  cFuncBg,  cFuncText,  false };
    g_buttons[0][4] = { (const VMWCHAR*)u"CE",   (const VMWCHAR*)u"CE",   cClearBg, cClearText, false };

    // Row 1: !, sin, cos, tan, /
    g_buttons[1][0] = { (const VMWCHAR*)u"!",    (const VMWCHAR*)u"!",    cFuncBg,  cFuncText,  false };
    g_buttons[1][1] = { (const VMWCHAR*)u"sin",  (const VMWCHAR*)u"sin(", cFuncBg,  cFuncText,  false };
    g_buttons[1][2] = { (const VMWCHAR*)u"cos",  (const VMWCHAR*)u"cos(", cFuncBg,  cFuncText,  false };
    g_buttons[1][3] = { (const VMWCHAR*)u"tan",  (const VMWCHAR*)u"tan(", cFuncBg,  cFuncText,  false };
    g_buttons[1][4] = { (const VMWCHAR*)u"/",    (const VMWCHAR*)u"/",    cOpBg,    cOpText,    false };

    // Row 2: (, ), M+, M, *
    g_buttons[2][0] = { (const VMWCHAR*)u"(",    (const VMWCHAR*)u"(",    cFuncBg,  cFuncText,  false };
    g_buttons[2][1] = { (const VMWCHAR*)u")",    (const VMWCHAR*)u")",    cFuncBg,  cFuncText,  false };
    g_buttons[2][2] = { (const VMWCHAR*)u"M+",   (const VMWCHAR*)u"M+",   cConstBg, cConstText, false };
    g_buttons[2][3] = { (const VMWCHAR*)u"M",    (const VMWCHAR*)u"M",    cConstBg, cConstText, false };
    g_buttons[2][4] = { (const VMWCHAR*)u"*",    (const VMWCHAR*)u"*",    cOpBg,    cOpText,    false };

    // Row 3: %, ln, abs, 10^x, -
    g_buttons[3][0] = { (const VMWCHAR*)u"%",    (const VMWCHAR*)u"%",    cOpBg,    cOpText,    false };
    g_buttons[3][1] = { (const VMWCHAR*)u"ln",   (const VMWCHAR*)u"ln(",  cFuncBg,  cFuncText,  false };
    g_buttons[3][2] = { (const VMWCHAR*)u"abs",  (const VMWCHAR*)u"abs(", cFuncBg,  cFuncText,  false };
    g_buttons[3][3] = { (const VMWCHAR*)u"10^x", (const VMWCHAR*)u"10^(", cFuncBg, cFuncText, false };
    g_buttons[3][4] = { (const VMWCHAR*)u"-",    (const VMWCHAR*)u"-",    cOpBg,    cOpText,    false };

    // Row 4: ², ³, 1/x, °, +
    g_buttons[4][0] = { (const VMWCHAR*)u"²",    (const VMWCHAR*)u"²",    cFuncBg,  cFuncText,  false };
    g_buttons[4][1] = { (const VMWCHAR*)u"³",    (const VMWCHAR*)u"³",    cFuncBg,  cFuncText,  false };
    g_buttons[4][2] = { (const VMWCHAR*)u"1/x",  (const VMWCHAR*)u"1/(",  cFuncBg,  cFuncText,  false };
    g_buttons[4][3] = { (const VMWCHAR*)u"°",    (const VMWCHAR*)u"°",    cFuncBg,  cFuncText,  false };
    g_buttons[4][4] = { (const VMWCHAR*)u"+",    (const VMWCHAR*)u"+",    cOpBg,    cOpText,    false };

    // Row 5: π, e, Ans, Pre, =
    g_buttons[5][0] = { (const VMWCHAR*)u"π",      (const VMWCHAR*)u"π",      cConstBg, cConstText, false };
    g_buttons[5][1] = { (const VMWCHAR*)u"e",      (const VMWCHAR*)u"e",      cConstBg, cConstText, false };
    g_buttons[5][2] = { (const VMWCHAR*)u"Ans",    (const VMWCHAR*)u"Ans",    cConstBg, cConstText, false };
    g_buttons[5][3] = { (const VMWCHAR*)u"Pre",    (const VMWCHAR*)u"PreAns", cConstBg, cConstText, false };
    g_buttons[5][4] = { (const VMWCHAR*)u"=",      (const VMWCHAR*)u"=",      cEqBg,    cEqText,    false };
}

static void ToggleSign(void)
{
    if (g_resultCalculated)
    {
        double val = -g_engine.Ans();
        g_engine.SetAns(val);
        FormatDoubleResult(val, g_result, KMaxResultLen);
        FormatDoubleResult(val, g_expression, KMaxExprLen);
        return;
    }

    int len = w_strlen(g_expression);
    if (len == 0)
    {
        AppendChar('-');
        return;
    }

    if (g_expression[len - 1] == '-')
    {
        g_expression[len - 1] = 0;
        return;
    }

    int i = len - 1;
    while (i >= 0 && ((g_expression[i] >= '0' && g_expression[i] <= '9') || g_expression[i] == '.'))
    {
        i--;
    }

    if (i < len - 1)
    {
        if (i >= 0 && g_expression[i] == '-')
        {
            for (int j = i; j < len; j++)
            {
                g_expression[j] = g_expression[j + 1];
            }
        }
        else
        {
            if (len < KMaxExprLen - 2)
            {
                for (int j = len; j > i + 1; j--)
                {
                    g_expression[j] = g_expression[j - 1];
                }
                g_expression[i + 1] = '-';
                g_expression[len + 1] = 0;
            }
        }
    }
    else
    {
        AppendChar('-');
    }
}

static void AppendChar(VMWCHAR ch)
{
    if (g_resultCalculated)
    {
        g_expression[0] = 0;
        g_result[0] = 0;
        g_resultCalculated = false;
        g_hasTemplateN = false;
    }
    int len = w_strlen(g_expression);
    if (len < KMaxExprLen - 2)
    {
        g_expression[len] = ch;
        g_expression[len + 1] = 0;
    }
}

static void AppendString(const VMWCHAR* str)
{
    if (g_resultCalculated)
    {
        g_expression[0] = 0;
        g_result[0] = 0;
        g_resultCalculated = false;
        g_hasTemplateN = false;
    }
    int curLen = w_strlen(g_expression);
    int addLen = w_strlen(str);
    if (curLen + addLen < KMaxExprLen - 2)
    {
        for (int i = 0; i < addLen; i++)
        {
            g_expression[curLen + i] = str[i];
        }
        g_expression[curLen + addLen] = 0;
    }
}

static void ClearEntry(void)
{
    g_expression[0] = 0;
    g_result[0] = 0;
    g_resultCalculated = false;
    g_hasTemplateN = false;
}

static bool StrEndsWith(const VMWCHAR* str, int strLen, const VMWCHAR* suffix)
{
    int sLen = w_strlen(suffix);
    if (strLen < sLen) return false;
    for (int i = 0; i < sLen; i++)
    {
        if (str[strLen - sLen + i] != suffix[i]) return false;
    }
    return true;
}

static const VMWCHAR* const kLinkedTokens[] = {
    (const VMWCHAR*)u"PreAns",
    (const VMWCHAR*)u"10^(",
    (const VMWCHAR*)u"sin(",
    (const VMWCHAR*)u"cos(",
    (const VMWCHAR*)u"tan(",
    (const VMWCHAR*)u"log(",
    (const VMWCHAR*)u"abs(",
    (const VMWCHAR*)u"1/(",
    (const VMWCHAR*)u"ln(",
    (const VMWCHAR*)u"Ans",
    (const VMWCHAR*)u"10^",
    (const VMWCHAR*)u"sin",
    (const VMWCHAR*)u"cos",
    (const VMWCHAR*)u"tan",
    (const VMWCHAR*)u"log",
    (const VMWCHAR*)u"abs",
    (const VMWCHAR*)u"1/",
    (const VMWCHAR*)u"ln",
    (const VMWCHAR*)u"Pre",
    NULL
};

static void DeleteChar(void)
{
    if (g_resultCalculated)
    {
        g_result[0] = 0;
        g_resultCalculated = false;
    }
    int len = w_strlen(g_expression);
    if (len <= 0) return;

    for (int i = 0; kLinkedTokens[i] != NULL; i++)
    {
        if (StrEndsWith(g_expression, len, kLinkedTokens[i]))
        {
            int tLen = w_strlen(kLinkedTokens[i]);
            g_expression[len - tLen] = 0;
            return;
        }
    }

    g_expression[len - 1] = 0;
}

static void CalculateResult(void)
{
    if (w_strlen(g_expression) == 0) return;
    if (g_isExitingGag) return;

    double resVal = 0.0;
    VMWCHAR errBuf[32] = {0};
    int err = g_engine.Evaluate(g_expression, resVal, errBuf);

    // Gag Easter Egg: Division / Modulo by 0 (e.g., 1/0, 1/(0)) plays SFX and exits after 2.25s
    if (err == -2)
    {
        g_isExitingGag = true;
        vm_set_volume(6); // Set max volume
        vm_audio_play_bytes((void*)kExplosionSfx, KExplosionSfxLen, VM_FORMAT_MP3, 0, VM_DEVICE_SPEAKER_BOTH, NULL);
        if (g_gagExitTimer != -1)
        {
            vm_delete_timer(g_gagExitTimer);
        }
        g_gagExitTimer = vm_create_timer(2250, OnExplosionExitTimer);
        return; // NO visual changes - keep screen as is
    }

    if (err == 0)
    {
        FormatDoubleResult(resVal, g_result, KMaxResultLen);
        g_engine.SetAns(resVal);
        g_engine.AddHistory(g_expression, g_result);
        g_resultCalculated = true;
        g_hasTemplateN = false;
    }
    else if (err == -3)
    {
        w_strcpy(g_result, (const VMWCHAR*)u"Math Error");
        g_resultCalculated = true;
    }
    else
    {
        w_strcpy(g_result, (const VMWCHAR*)u"Syntax Error");
        g_resultCalculated = true;
    }
}

static void ExecuteButton(int col, int row)
{
    if (col < 0 || col >= KGridCols || row < 0 || row >= KGridRows) return;

    TriggerButtonFade(col, row);

    // Row 0, Col 3: DEG / RAD Toggle
    if (row == 0 && col == 3)
    {
        g_engine.ToggleAngleMode();
        return;
    }

    const TGridBtn& btn = g_buttons[row][col];
    const VMWCHAR* lbl = btn.iLabel;

    // "="
    if (lbl[0] == '=' && lbl[1] == 0)
    {
        CalculateResult();
        return;
    }
    // "CE"
    if (lbl[0] == 'C' && lbl[1] == 'E' && lbl[2] == 0)
    {
        ClearEntry();
        return;
    }
    // "M+"
    if (lbl[0] == 'M' && lbl[1] == '+' && lbl[2] == 0)
    {
        if (g_resultCalculated && g_result[0] != 0 && g_result[0] != 'S')
        {
            g_engine.AddToMemory(g_engine.Ans());
        }
        else
        {
            double val = 0.0;
            VMWCHAR errBuf[32];
            if (g_engine.Evaluate(g_expression, val, errBuf) == 0)
            {
                g_engine.AddToMemory(val);
                FormatDoubleResult(val, g_result, KMaxResultLen);
                g_resultCalculated = true;
            }
        }
        return;
    }
    // "M" (Recall Memory)
    if (lbl[0] == 'M' && lbl[1] == 0)
    {
        AppendString((const VMWCHAR*)u"M");
        return;
    }

    // Note 1: ⁿ√ and logₙ template insert
    if (btn.iIsTemplateN)
    {
        if (lbl[0] == 0x207F) // ⁿ√
        {
            AppendString((const VMWCHAR*)u"ⁿ√(");
            g_hasTemplateN = true;
        }
        else // logₙ
        {
            AppendString((const VMWCHAR*)u"logₙ(");
            g_hasTemplateN = true;
        }
        return;
    }

    AppendString(btn.iAction);
}

// -----------------------------------------------------------------------------
// Rendering Code
// -----------------------------------------------------------------------------
static void SetDrawColor(VMUINT16 c565)
{
    vm_graphic_color col;
    col.vm_color_565 = c565;
    vm_graphic_setcolor(&col);
}

static void DrawRoundRect(VMINT layer, int x, int y, int w, int h, int r, VMUINT16 borderCol, VMUINT16 fillCol)
{
    if (y >= KScreenHeight || y + h <= 0 || x >= KScreenWidth || x + w <= 0) return;
    SetDrawColor(fillCol);
    vm_graphic_fill_rect_ex(layer, x, y, w, h);
    SetDrawColor(borderCol);
    vm_graphic_rect_ex(layer, x, y, w, h);
}

// 1-pixel clipped rounded corner rectangle
static void DrawClippedRect(VMINT layer, int x, int y, int w, int h, VMUINT16 borderCol, VMUINT16 fillCol)
{
    if (y >= KScreenHeight || y + h <= 0 || x >= KScreenWidth || x + w <= 0) return;

    // Top row (corners clipped by 1px)
    SetDrawColor(fillCol);
    if (y >= 0 && y < KScreenHeight)
    {
        vm_graphic_fill_rect_ex(layer, x + 1, y, w - 2, 1);
    }

    // Middle rows
    if (y + 1 < KScreenHeight && y + h - 1 > 0)
    {
        int my = (y + 1 < 0) ? 0 : y + 1;
        int mh = (y + h - 1 > KScreenHeight) ? (KScreenHeight - my) : (y + h - 1 - my);
        if (mh > 0)
        {
            vm_graphic_fill_rect_ex(layer, x, my, w, mh);
        }
    }

    // Bottom row (corners clipped by 1px)
    if (y + h - 1 >= 0 && y + h - 1 < KScreenHeight)
    {
        vm_graphic_fill_rect_ex(layer, x + 1, y + h - 1, w - 2, 1);
    }

    // 1-pixel clipped border lines (corners removed)
    SetDrawColor(borderCol);
    if (y >= 0 && y < KScreenHeight)
    {
        vm_graphic_line_ex(layer, x + 1, y, x + w - 2, y); // Top border
    }
    if (y + h - 1 >= 0 && y + h - 1 < KScreenHeight)
    {
        vm_graphic_line_ex(layer, x + 1, y + h - 1, x + w - 2, y + h - 1); // Bottom border
    }
    int ly1 = (y + 1 < 0) ? 0 : y + 1;
    int ly2 = (y + h - 2 >= KScreenHeight) ? (KScreenHeight - 1) : y + h - 2;
    if (ly2 >= ly1)
    {
        vm_graphic_line_ex(layer, x, ly1, x, ly2);                 // Left border
        vm_graphic_line_ex(layer, x + w - 1, ly1, x + w - 1, ly2); // Right border
    }
}

// 1-pixel clipped rounded corner button with Windows 7 Aero two-tone split
static void DrawClippedButton(VMINT layer, int x, int y, int w, int h, VMUINT16 borderCol, VMUINT16 fillTop, VMUINT16 fillBottom)
{
    if (y >= KScreenHeight || y + h <= 0 || x >= KScreenWidth || x + w <= 0) return;
    int halfH = h / 2; // 16px

    // Top half (lighter aero glass tone)
    SetDrawColor(fillTop);
    vm_graphic_fill_rect_ex(layer, x + 1, y, w - 2, 1);
    vm_graphic_fill_rect_ex(layer, x, y + 1, w, halfH - 1);

    // Bottom half (deeper aero glass base)
    SetDrawColor(fillBottom);
    vm_graphic_fill_rect_ex(layer, x, y + halfH, w, h - halfH - 1);
    vm_graphic_fill_rect_ex(layer, x + 1, y + h - 1, w - 2, 1);

    // 1-pixel clipped border lines (corners removed)
    SetDrawColor(borderCol);
    vm_graphic_line_ex(layer, x + 1, y, x + w - 2, y);                 // Top border
    vm_graphic_line_ex(layer, x + 1, y + h - 1, x + w - 2, y + h - 1); // Bottom border
    vm_graphic_line_ex(layer, x, y + 1, x, y + h - 2);                 // Left border
    vm_graphic_line_ex(layer, x + w - 1, y + 1, x + w - 1, y + h - 2); // Right border
}

static inline int GetColX(int c)
{
    // Col 0: 4, Col 1: 51, Col 2: 98, Col 3: 145, Col 4: 193
    static const int s_colX[5] = {4, 51, 98, 145, 193};
    if (c >= 0 && c < 5) return s_colX[c];
    return 4;
}

static inline int GetColW(int c)
{
    // Col 3 (4th column) extended by 1px to 44px; other columns 43px
    return (c == 3) ? 44 : 43;
}

static void DrawTitleBar(VMINT layer, int offsetY, const VMWCHAR* titleText)
{
    int y = offsetY;
    if (y >= KScreenHeight || y + 34 <= 0) return;

    // Windows 7 Aero Glass Title Bar (Height: 34px, Width: 240px)
    // Top highlight line
    if (y >= 0 && y < KScreenHeight)
    {
        SetDrawColor(KColor_RGB(232, 242, 254));
        vm_graphic_line_ex(layer, 0, y, KScreenWidth - 1, y);
    }

    // Top half tone (16px)
    if (y + 1 < KScreenHeight && y + 17 > 0)
    {
        int fy = (y + 1 < 0) ? 0 : y + 1;
        int fh = (y + 17 > KScreenHeight) ? (KScreenHeight - fy) : (y + 17 - fy);
        if (fh > 0)
        {
            SetDrawColor(KColor_RGB(206, 224, 246));
            vm_graphic_fill_rect_ex(layer, 0, fy, KScreenWidth, fh);
        }
    }

    // Bottom half tone (16px)
    if (y + 17 < KScreenHeight && y + 33 > 0)
    {
        int fy = (y + 17 < 0) ? 0 : y + 17;
        int fh = (y + 33 > KScreenHeight) ? (KScreenHeight - fy) : (y + 33 - fy);
        if (fh > 0)
        {
            SetDrawColor(KColor_RGB(180, 204, 234));
            vm_graphic_fill_rect_ex(layer, 0, fy, KScreenWidth, fh);
        }
    }

    // Bottom border line
    if (y + 33 >= 0 && y + 33 < KScreenHeight)
    {
        SetDrawColor(KColor_RGB(145, 172, 204));
        vm_graphic_line_ex(layer, 0, y + 33, KScreenWidth - 1, y + 33);
    }

    // Draw Windows 7 Calculator Icon (24x24) from install.wim (vertically centered in 34px bar: Y = y + 5)
    VMUINT16* fb = (VMUINT16*)vm_graphic_get_layer_buffer(layer);
    if (fb != NULL)
    {
        int iconX = 8;
        int iconY = y + (34 - KCALC_ICON_H) / 2;

        for (int iy = 0; iy < KCALC_ICON_H; iy++)
        {
            int py = iconY + iy;
            if (py < 0 || py >= KScreenHeight) continue;

            for (int ix = 0; ix < KCALC_ICON_W; ix++)
            {
                int px = iconX + ix;
                if (px < 0 || px >= KScreenWidth) continue;

                VMUINT32 argb = KCalcIconData[iy * KCALC_ICON_W + ix];
                VMUINT32 alpha = (argb >> 24) & 0xFF;
                if (alpha == 0) continue;

                VMUINT32 r = (argb >> 16) & 0xFF;
                VMUINT32 g = (argb >> 8) & 0xFF;
                VMUINT32 b = argb & 0xFF;
                VMUINT16 icon565 = KColor_RGB(r, g, b);

                if (alpha == 255)
                {
                    fb[py * KScreenWidth + px] = icon565;
                }
                else
                {
                    VMUINT16 bg565 = fb[py * KScreenWidth + px];
                    fb[py * KScreenWidth + px] = BlendRGB565(bg565, icon565, alpha);
                }
            }
        }
    }

    // Title Text (vertically centered in 34px bar, to the right of the icon)
    vm_graphic_set_font(VM_SMALL_FONT);
    vm_font_set_font_size(VM_SMALL_FONT);
    SetDrawColor(KColor_RGB(20, 45, 85));

    int txtH = vm_graphic_get_character_height();
    if (txtH <= 0 || txtH > 34) txtH = 14;
    int txtY = y + (34 - txtH) / 2;
    int txtX = 8 + KCALC_ICON_W + 6; // 38px

    if (txtY >= 0 && txtY < KScreenHeight - txtH)
    {
        vm_graphic_textout_to_layer(layer, txtX, txtY, (VMWSTR)titleText, w_strlen(titleText));
    }
}

static void RenderNormalView(void)
{
    // 1. Title Bar (Height: 34px, Width: 240px with Windows 7 Icon)
    DrawTitleBar(g_layer, 0, (const VMWCHAR*)u"Calculator");

    // Force system small font
    vm_graphic_set_font(VM_SMALL_FONT);
    vm_font_set_font_size(VM_SMALL_FONT);

    // 2. Top Display Box (Y: 38 to 78, Height: 40px) - Crisp Aero White LCD Box
    DrawRoundRect(g_layer, 4, 38, 232, 40, 4, KColor_RGB(145, 168, 192), KColor_RGB(255, 255, 255));

    // Status indicator inside display ([M] only, DEG/RAD is shown on dedicated button)
    if (g_engine.HasMemory())
    {
        SetDrawColor(KColor_RGB(45, 90, 155));
        vm_graphic_textout_to_layer(g_layer, 8, 41, (VMWSTR)u"[M]", 3);
    }

    // Expression line (Y: 41)
    SetDrawColor(KColor_RGB(95, 110, 130));
    const VMWCHAR* exprPtr = (g_expression[0] != 0) ? g_expression : (const VMWCHAR*)u"0";
    int exprW = vm_graphic_get_string_width((VMWSTR)exprPtr);
    int exprX = 228 - exprW;
    if (exprX < 10) exprX = 10;
    vm_graphic_textout_to_layer(g_layer, exprX, 41, (VMWSTR)exprPtr, w_strlen(exprPtr));

    // Result line (Y: 59)
    if (g_result[0] != 0)
    {
        SetDrawColor(KColor_RGB(15, 20, 30)); // Deep Charcoal / Black
        int resW = vm_graphic_get_string_width((VMWSTR)g_result);
        int resX = 228 - resW;
        if (resX < 10) resX = 10;
        vm_graphic_textout_to_layer(g_layer, resX, 59, (VMWSTR)g_result, w_strlen(g_result));
    }

    // 3. 6x5 Keypad Grid (Y: 82 to 292 - strictly stationary)
    int btnH = 32;
    int startY = 82;
    int gapY = 3;
    int fontH = vm_graphic_get_character_height();
    if (fontH <= 0 || fontH > btnH) fontH = 14;

    for (int r = 0; r < KGridRows; r++)
    {
        for (int c = 0; c < KGridCols; c++)
        {
            int bw = GetColW(c);
            int bx = GetColX(c);
            int by = startY + r * (btnH + gapY);
            const TGridBtn& btn = g_buttons[r][c];

            bool isPressed = (c == g_pressedCol && r == g_pressedRow);
            float glow = g_btnGlow[r][c];

            VMUINT16 borderCol = KColor_RGB(160, 182, 208);
            VMUINT16 topCol = BlendRGB565(btn.iBgColor, KColor_RGB(255, 255, 255), 140);
            VMUINT16 botCol = btn.iBgColor;
            VMUINT16 textCol = btn.iTextColor;

            if (isPressed)
            {
                topCol = KColor_RGB(220, 240, 255); // Aero Active Press Top (Light Sky Blue)
                botCol = KColor_RGB(165, 205, 250); // Aero Active Press Bottom
                borderCol = KColor_RGB(30, 120, 220);
                textCol = KColor_RGB(0, 0, 0);
            }
            else if (glow > 0.001f)
            {
                int t = (int)(glow * 256.0f);
                topCol = BlendRGB565(topCol, KColor_RGB(255, 252, 230), t); // Glow Top (Luminous Light Gold)
                botCol = BlendRGB565(botCol, KColor_RGB(255, 230, 160), t); // Glow Bottom (Rich Warm Gold)
                borderCol = BlendRGB565(KColor_RGB(160, 182, 208), KColor_RGB(230, 150, 15), t);
                textCol = BlendRGB565(btn.iTextColor, KColor_RGB(0, 0, 0), t);
            }

            DrawClippedButton(g_layer, bx, by, bw, btnH, borderCol, topCol, botCol);

            // Dynamic Button label (DEG / RAD changes based on mode)
            const VMWCHAR* labelText = btn.iLabel;
            if (r == 0 && c == 3)
            {
                labelText = (g_engine.AngleMode() == EAngleDegrees) ? (const VMWCHAR*)u"DEG" : (const VMWCHAR*)u"RAD";
            }

            SetDrawColor(textCol);
            int txtW = vm_graphic_get_string_width((VMWSTR)labelText);
            int txtX = bx + (bw - txtW) / 2;
            int txtY = by + (btnH - fontH) / 2;
            if (txtX < bx + 1) txtX = bx + 1;
            vm_graphic_textout_to_layer(g_layer, txtX, txtY, (VMWSTR)labelText, w_strlen(labelText));
        }
    }

    // 4. Bottom Softkey Bar (Y: 296 to 319) - Aero Frosted Bar
    DrawRoundRect(g_layer, 0, 296, KScreenWidth, 24, 0, KColor_RGB(165, 188, 215), KColor_RGB(205, 220, 238));

    SetDrawColor(KColor_RGB(30, 60, 105)); // Left Softkey: History
    vm_graphic_textout_to_layer(g_layer, 6, 300, (VMWSTR)u"History", 7);

    // Right Softkey: "Clear" if expression has characters, "Exit" if empty
    bool hasChars = (w_strlen(g_expression) > 0 || g_result[0] != 0);
    const VMWCHAR* rskLabel = hasChars ? (const VMWCHAR*)u"Clear" : (const VMWCHAR*)u"Exit";
    SetDrawColor(KColor_RGB(165, 35, 45));
    int rskW = vm_graphic_get_string_width((VMWSTR)rskLabel);
    vm_graphic_textout_to_layer(g_layer, KScreenWidth - rskW - 6, 300, (VMWSTR)rskLabel, w_strlen(rskLabel));
}

static void RenderHistoryView(int offsetY)
{
    // Background overlay container (covers the underlying calculator view)
    int bgY = (offsetY < 0) ? 0 : offsetY;
    int bgH = (bgY < KScreenHeight) ? (KScreenHeight - bgY) : 0;
    if (bgH > 0)
    {
        SetDrawColor(KColor_RGB(218, 228, 242));
        vm_graphic_fill_rect_ex(g_layer, 0, bgY, KScreenWidth, bgH);
    }

    // 1. Title Bar (Height: 34px, Width: 240px with Windows 7 Icon)
    DrawTitleBar(g_layer, offsetY, (const VMWCHAR*)u"History");

    // Force system small font
    vm_graphic_set_font(VM_SMALL_FONT);
    vm_font_set_font_size(VM_SMALL_FONT);

    // 2. List container (Y: 38 to 292, Height: 254px - 1px clipped corners)
    DrawClippedRect(g_layer, 4, 38 + offsetY, 232, 254, KColor_RGB(165, 185, 208), KColor_RGB(248, 250, 254));

    int total = g_engine.HistoryCount();
    if (total == 0)
    {
        int msgY = 138 + offsetY;
        if (msgY >= 0 && msgY < KScreenHeight - 14)
        {
            SetDrawColor(KColor_RGB(120, 135, 155));
            vm_graphic_textout_to_layer(g_layer, 60, msgY, (VMWSTR)u"(No history yet)", 16);
        }
    }
    else
    {
        int itemH = 48;
        int maxVisible = 5;
        if (g_historyTop < 0) g_historyTop = 0;
        if (g_historyTop > total - 1) g_historyTop = total - 1;

        int curY = 42 + offsetY;
        for (int i = g_historyTop; i < total && i < g_historyTop + maxVisible; i++)
        {
            const THistoryItem& item = g_engine.HistoryItem(i);
            float glow = (i >= 0 && i < KMaxHistoryGlow) ? g_historyGlow[i] : 0.0f;

            VMUINT16 borderCol = KColor_RGB(190, 205, 225);
            VMUINT16 bgCol = KColor_RGB(236, 242, 250);

            if (glow > 0.001f)
            {
                int t = (int)(glow * 256.0f);
                bgCol = BlendRGB565(KColor_RGB(236, 242, 250), KColor_RGB(255, 245, 200), t);
                borderCol = BlendRGB565(KColor_RGB(190, 205, 225), KColor_RGB(240, 160, 20), t);
            }

            if (curY < KScreenHeight && curY + itemH > 0)
            {
                DrawClippedRect(g_layer, 8, curY, 224, itemH - 4, borderCol, bgCol);

                // Expression
                if (curY + 4 >= 0 && curY + 4 < KScreenHeight - 14)
                {
                    SetDrawColor(KColor_RGB(60, 75, 95));
                    vm_graphic_textout_to_layer(g_layer, 12, curY + 4, (VMWSTR)item.iExpression, w_strlen(item.iExpression));
                }

                // Result
                if (curY + 22 >= 0 && curY + 22 < KScreenHeight - 14)
                {
                    SetDrawColor(KColor_RGB(20, 120, 50));
                    VMWCHAR resLine[KMaxResultLen + 4] = {0};
                    resLine[0] = '=';
                    resLine[1] = ' ';
                    w_strcpy(resLine + 2, item.iResult, KMaxResultLen);
                    int rw = vm_graphic_get_string_width((VMWSTR)resLine);
                    int rx = 224 - rw;
                    if (rx < 12) rx = 12;
                    vm_graphic_textout_to_layer(g_layer, rx, curY + 22, (VMWSTR)resLine, w_strlen(resLine));
                }
            }

            curY += itemH;
        }
    }

    // 3. Bottom Softkeys in History View (LSK: Clr&Close, RSK: Back)
    int skY = 296 + offsetY;
    if (skY < KScreenHeight && skY + 24 > 0)
    {
        DrawRoundRect(g_layer, 0, skY, KScreenWidth, 24, 0, KColor_RGB(165, 188, 215), KColor_RGB(205, 220, 238));

        if (skY + 4 >= 0 && skY + 4 < KScreenHeight - 14)
        {
            SetDrawColor(KColor_RGB(165, 35, 45)); // Left Softkey: Clr&Close
            vm_graphic_textout_to_layer(g_layer, 6, skY + 4, (VMWSTR)u"Clr&Close", 9);

            SetDrawColor(KColor_RGB(30, 60, 105)); // Right Softkey: Back
            int backW = vm_graphic_get_string_width((VMWSTR)u"Back");
            vm_graphic_textout_to_layer(g_layer, KScreenWidth - backW - 6, skY + 4, (VMWSTR)u"Back", 4);
        }
    }
}

static void RenderScreen(void)
{
    if (g_layer == -1) return;

    // Windows 7 Aero Glass Background fill
    SetDrawColor(KColor_RGB(218, 228, 242));
    vm_graphic_fill_rect_ex(g_layer, 0, 0, KScreenWidth, KScreenHeight);

    if (g_slideState == ESlideNone)
    {
        if (g_viewMode == EViewNormal)
        {
            RenderNormalView();
        }
        else
        {
            RenderHistoryView(0);
        }
    }
    else
    {
        // Overlay slide transition:
        // 1. Calculator view remains stationary in background
        RenderNormalView();
        // 2. History view slides over vertically as an overlay
        RenderHistoryView(g_slideOffsetY);
    }

    // Flush layer to screen
    vm_graphic_flush_layer(&g_layer, 1);
}

// -----------------------------------------------------------------------------
// Events Handling
// -----------------------------------------------------------------------------
void handle_sysevt(VMINT message, VMINT param)
{
    switch (message)
    {
    case VM_MSG_CREATE:
    case VM_MSG_ACTIVE:
    case VM_MSG_PAINT:
        if (g_layer == -1)
        {
            g_layer = vm_graphic_create_layer(0, 0, KScreenWidth, KScreenHeight, -1);
            vm_graphic_set_clip(0, 0, KScreenWidth, KScreenHeight);
        }
        RenderScreen();
        break;

    case VM_MSG_INACTIVE:
    case VM_MSG_HIDE:
        ResetKeyHoldState();
        if (g_layer != -1)
        {
            vm_graphic_delete_layer(g_layer);
            g_layer = -1;
        }
        break;

    case VM_MSG_QUIT:
        ResetKeyHoldState();
        if (g_layer != -1)
        {
            vm_graphic_delete_layer(g_layer);
            g_layer = -1;
        }
        vm_res_deinit();
        break;
    }
}

void handle_keyevt(VMINT event, VMINT keycode)
{
    if (g_isExitingGag || g_slideState != ESlideNone) return;

    // 1. Directional D-Pad keys (UP, DOWN, LEFT, RIGHT)
    // Supports holding, simultaneous multi-key diagonal navigation, and opposite-key cancellation
    if (keycode == VM_KEY_UP || keycode == VM_KEY_DOWN || keycode == VM_KEY_LEFT || keycode == VM_KEY_RIGHT)
    {
        if (event == VM_KEY_EVENT_DOWN)
        {
            if (keycode == VM_KEY_UP) g_keyUpHeld = true;
            else if (keycode == VM_KEY_DOWN) g_keyDownHeld = true;
            else if (keycode == VM_KEY_LEFT) g_keyLeftHeld = true;
            else if (keycode == VM_KEY_RIGHT) g_keyRightHeld = true;

            int dx = 0, dy = 0;
            GetNetDirection(dx, dy);
            if (dx != 0 || dy != 0)
            {
                StepDirectionalNavigation(dx, dy);
            }

            // Start initial hold delay timer (333ms)
            if (g_keyHoldTimer != -1)
            {
                vm_delete_timer(g_keyHoldTimer);
                g_keyHoldTimer = -1;
            }
            g_keyHoldInRepeat = false;
            g_keyHoldTimer = vm_create_timer(KKeyHoldInitialDelayMs, OnKeyHoldTimerTick);
            return;
        }
        else if (event == VM_KEY_EVENT_UP)
        {
            if (keycode == VM_KEY_UP) g_keyUpHeld = false;
            else if (keycode == VM_KEY_DOWN) g_keyDownHeld = false;
            else if (keycode == VM_KEY_LEFT) g_keyLeftHeld = false;
            else if (keycode == VM_KEY_RIGHT) g_keyRightHeld = false;

            if (!g_keyUpHeld && !g_keyDownHeld && !g_keyLeftHeld && !g_keyRightHeld)
            {
                if (g_keyHoldTimer != -1)
                {
                    vm_delete_timer(g_keyHoldTimer);
                    g_keyHoldTimer = -1;
                }
                g_keyHoldInRepeat = false;
            }
            return;
        }
        return; // Ignore REPEAT for D-Pad since custom timer manages auto-repeat
    }

    // 2. Non-directional keys respond to KEY DOWN (and REPEAT for backspace/delete)
    if (event != VM_KEY_EVENT_DOWN && event != VM_KEY_EVENT_REPEAT)
    {
        return;
    }

    // Ignore REPEAT for single-action triggers like OK and softkeys
    if (event == VM_KEY_EVENT_REPEAT && keycode != VM_KEY_CLEAR && keycode != VM_KEY_BACK && keycode != VM_KEY_BACKSPACE && keycode != VM_KEY_DEL && keycode != VM_KEY_RIGHT_SOFTKEY)
    {
        return;
    }

    if (g_viewMode == EViewHistory)
    {
        if (keycode == VM_KEY_OK) // Select history item
        {
            int total = g_engine.HistoryCount();
            if (total > 0 && g_historyFocus >= 0 && g_historyFocus < total)
            {
                const THistoryItem& item = g_engine.HistoryItem(g_historyFocus);
                w_strcpy(g_expression, item.iExpression);
                w_strcpy(g_result, item.iResult);
                g_resultCalculated = true;
                CloseHistoryWithAnimation();
            }
            return;
        }
        if (keycode == VM_KEY_LEFT_SOFTKEY) // Clr&Close
        {
            g_engine.ClearHistory();
            g_historyFocus = 0;
            g_historyTop = 0;
            for (int i = 0; i < KMaxHistoryGlow; i++) g_historyGlow[i] = 0.0f;
            CloseHistoryWithAnimation();
            return;
        }
        if (keycode == VM_KEY_RIGHT_SOFTKEY || keycode == VM_KEY_BACK || keycode == VM_KEY_CLEAR) // Back
        {
            CloseHistoryWithAnimation();
            return;
        }
        return;
    }

    // Normal Mode Key Handlers
    if (keycode == VM_KEY_OK) // Center D-Pad
    {
        ExecuteButton(g_selectedCol, g_selectedRow);
        RenderScreen();
        return;
    }

    // Softkeys
    if (keycode == VM_KEY_LEFT_SOFTKEY) // Open History with slide animation
    {
        OpenHistoryWithAnimation();
        return;
    }
    if (keycode == VM_KEY_RIGHT_SOFTKEY) // RSK: Backspace until empty, then Exit
    {
        if (w_strlen(g_expression) > 0 || g_result[0] != 0)
        {
            DeleteChar();
            RenderScreen();
        }
        else
        {
            if (event == VM_KEY_EVENT_DOWN)
            {
                vm_exit_app();
            }
        }
        return;
    }

    // Physical Numeric Keys 0-9
    if (keycode >= VM_KEY_NUM0 && keycode <= VM_KEY_NUM9)
    {
        if (event == VM_KEY_EVENT_DOWN)
        {
            AppendChar((VMWCHAR)('0' + (keycode - VM_KEY_NUM0)));
            RenderScreen();
        }
        return;
    }

    // Other Physical Keys
    if (keycode == VM_KEY_POUND) // # enters .
    {
        if (event == VM_KEY_EVENT_DOWN)
        {
            AppendChar('.');
            RenderScreen();
        }
        return;
    }
    if (keycode == VM_KEY_STAR) // * enters +/-
    {
        if (event == VM_KEY_EVENT_DOWN)
        {
            ToggleSign();
            RenderScreen();
        }
        return;
    }
    if (keycode == VM_KEY_CLEAR || keycode == VM_KEY_BACK || keycode == VM_KEY_BACKSPACE || keycode == VM_KEY_DEL)
    {
        DeleteChar();
        RenderScreen();
        return;
    }
}

void handle_penevt(VMINT event, VMINT x, VMINT y)
{
    if (event != VM_PEN_EVENT_TAP || g_isExitingGag || g_slideState != ESlideNone) return;

    if (g_viewMode == EViewHistory)
    {
        // Touch on LSK (Clr&Close)
        if (y >= 290 && x <= 100)
        {
            g_engine.ClearHistory();
            g_historyFocus = 0;
            g_historyTop = 0;
            for (int i = 0; i < KMaxHistoryGlow; i++) g_historyGlow[i] = 0.0f;
            CloseHistoryWithAnimation();
            return;
        }
        // Touch on RSK (Back)
        if (y >= 290 && x >= 140)
        {
            CloseHistoryWithAnimation();
            return;
        }
        // Touch on History Item
        if (y >= 42 && y < 290)
        {
            int idx = (y - 42) / 48 + g_historyTop;
            int total = g_engine.HistoryCount();
            if (idx >= 0 && idx < total)
            {
                const THistoryItem& item = g_engine.HistoryItem(idx);
                w_strcpy(g_expression, item.iExpression);
                w_strcpy(g_result, item.iResult);
                g_resultCalculated = true;
                CloseHistoryWithAnimation();
            }
        }
        return;
    }

    // Touch on Softkeys
    if (y >= 290 && x <= 100) // LSK
    {
        OpenHistoryWithAnimation();
        return;
    }
    if (y >= 290 && x >= 140) // RSK
    {
        if (w_strlen(g_expression) > 0 || g_result[0] != 0)
        {
            DeleteChar();
            RenderScreen();
        }
        else
        {
            vm_exit_app();
        }
        return;
    }

    // Touch on Angle Mode DEG/RAD in Display area
    if (y >= 38 && y <= 78 && x <= 80)
    {
        g_engine.ToggleAngleMode();
        RenderScreen();
        return;
    }

    // Touch on 6x5 Keypad Grid
    int btnH = 32;
    int startY = 82;
    int gapY = 3;

    for (int r = 0; r < KGridRows; r++)
    {
        for (int c = 0; c < KGridCols; c++)
        {
            int bw = GetColW(c);
            int bx = GetColX(c);
            int by = startY + r * (btnH + gapY);

            if (x >= bx && x <= bx + bw && y >= by && y <= by + btnH)
            {
                g_selectedCol = c;
                g_selectedRow = r;
                ExecuteButton(c, r);
                RenderScreen();
                return;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Entry Point
// -----------------------------------------------------------------------------
void vm_main(void)
{
    g_layer = -1;

    vm_graphic_set_font(VM_SMALL_FONT);
    vm_font_set_font_size(VM_SMALL_FONT);

    InitButtons();
    ClearEntry();

    vm_reg_sysevt_callback(handle_sysevt);
    vm_reg_keyboard_callback(handle_keyevt);
    vm_reg_pen_callback(handle_penevt);

    vm_res_init();
}
