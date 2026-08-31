#ifndef CALCULATOR_ENGINE_H
#define CALCULATOR_ENGINE_H

#include "vmsys.h"
#include "vmstdlib.h"
#include <math.h>

#define KMaxHistoryItems 20
#define KMaxExprLen 256
#define KMaxResultLen 64

enum TAngleMode
{
    EAngleDegrees = 0,
    EAngleRadians = 1
};

struct THistoryItem
{
    VMWCHAR iExpression[KMaxExprLen];
    VMWCHAR iResult[KMaxResultLen];
};

class CalculatorEngine
{
public:
    CalculatorEngine();
    ~CalculatorEngine();

    // Configuration & State
    TAngleMode AngleMode() const { return iAngleMode; }
    void SetAngleMode(TAngleMode aMode) { iAngleMode = aMode; }
    void ToggleAngleMode() { iAngleMode = (iAngleMode == EAngleDegrees) ? EAngleRadians : EAngleDegrees; }

    // Memory
    bool HasMemory() const { return iHasMemory; }
    double MemoryValue() const { return iMemoryValue; }
    void AddToMemory(double aVal);
    void ClearMemory();

    // Ans & PreAns
    double Ans() const { return iAns; }
    double PreAns() const { return iPreAns; }
    void SetAns(double aVal);

    // Evaluation
    int Evaluate(const VMWCHAR* aExpression, double& aOutResult, VMWCHAR* aOutError);

    // History
    int HistoryCount() const { return iHistoryCount; }
    const THistoryItem& HistoryItem(int aIndex) const { return iHistory[aIndex]; }
    void AddHistory(const VMWCHAR* aExpr, const VMWCHAR* aRes);
    void ClearHistory() { iHistoryCount = 0; }

private:
    double Factorial(double n);
    double ApplyFunction(const char* func, double arg1, double arg2, bool hasArg2, int& err);

    TAngleMode iAngleMode;
    double iMemoryValue;
    bool iHasMemory;
    double iAns;
    double iPreAns;

    THistoryItem iHistory[KMaxHistoryItems];
    int iHistoryCount;
};

#endif // CALCULATOR_ENGINE_H
