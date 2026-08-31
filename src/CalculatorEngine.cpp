#include "CalculatorEngine.h"
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef M_E
#define M_E 2.71828182845904523536
#endif

static int wstr_len(const VMWCHAR* s)
{
    int len = 0;
    while (s && s[len] != 0) len++;
    return len;
}

static void wstr_cpy(VMWCHAR* dest, const VMWCHAR* src, int maxLen)
{
    int i = 0;
    if (!dest || !src) return;
    while (src[i] != 0 && i < maxLen - 1)
    {
        dest[i] = src[i];
        i++;
    }
    dest[i] = 0;
}

CalculatorEngine::CalculatorEngine()
    : iAngleMode(EAngleDegrees),
      iMemoryValue(0.0),
      iHasMemory(false),
      iAns(0.0),
      iPreAns(0.0),
      iHistoryCount(0)
{
}

CalculatorEngine::~CalculatorEngine()
{
}

void CalculatorEngine::AddToMemory(double aVal)
{
    iMemoryValue += aVal;
    iHasMemory = true;
}

void CalculatorEngine::ClearMemory()
{
    iMemoryValue = 0.0;
    iHasMemory = false;
}

void CalculatorEngine::SetAns(double aVal)
{
    iPreAns = iAns;
    iAns = aVal;
}

void CalculatorEngine::AddHistory(const VMWCHAR* aExpr, const VMWCHAR* aRes)
{
    if (!aExpr || aExpr[0] == 0 || !aRes || aRes[0] == 0) return;

    // Prevent duplicate entries if the exact same equation is evaluated repeatedly
    if (iHistoryCount > 0)
    {
        const THistoryItem& last = iHistory[iHistoryCount - 1];
        bool same = true;
        for (int i = 0; aExpr[i] != 0 || last.iExpression[i] != 0; i++)
        {
            if (aExpr[i] != last.iExpression[i]) { same = false; break; }
        }
        if (same)
        {
            for (int i = 0; aRes[i] != 0 || last.iResult[i] != 0; i++)
            {
                if (aRes[i] != last.iResult[i]) { same = false; break; }
            }
            if (same) return;
        }
    }

    if (iHistoryCount >= KMaxHistoryItems)
    {
        memmove(&iHistory[0], &iHistory[1], (KMaxHistoryItems - 1) * sizeof(THistoryItem));
        iHistoryCount = KMaxHistoryItems - 1;
    }
    wstr_cpy(iHistory[iHistoryCount].iExpression, aExpr, KMaxExprLen);
    wstr_cpy(iHistory[iHistoryCount].iResult, aRes, KMaxResultLen);
    iHistoryCount++;
}

double CalculatorEngine::Factorial(double n)
{
    if (n < 0 || n > 170 || floor(n) != n) return -1.0;
    double r = 1.0;
    int intN = (int)n;
    for (int i = 2; i <= intN; i++)
    {
        r *= (double)i;
    }
    return r;
}

// Token types for Shunting-Yard
enum TokenType
{
    TOK_NUM,
    TOK_OP,
    TOK_FUNC,
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_COMMA
};

struct Token
{
    TokenType type;
    double val;
    char op;       // '+', '-', '*', '/', '^', '!'
    char func[16]; // "sin", "cos", "tan", "log", "ln", "sqrt", "cbrt", "nroot", "logn"
    int precedence;
    bool rightAssoc;
};

// Static buffers to prevent stack overflow on ARM thread stack
static Token s_tokens[128];
static Token s_rpn[128];
static Token s_opStack[64];
static double s_valStack[64];

static int GetPrecedence(char op)
{
    switch (op)
    {
    case '+': case '-': return 1;
    case '*': case '/': return 2;
    case '^': return 3;
    case '!': case '%': case 's': return 4;
    default: return 0;
    }
}

#define CHECK_INSERT_IMPLICIT_MUL() \
    do { \
        if (!mayBeUnary && tokenCount < 128) { \
            tokens[tokenCount].type = TOK_OP; \
            tokens[tokenCount].op = '*'; \
            tokens[tokenCount].precedence = GetPrecedence('*'); \
            tokens[tokenCount].rightAssoc = false; \
            tokenCount++; \
            mayBeUnary = true; \
        } \
    } while (0)

int CalculatorEngine::Evaluate(const VMWCHAR* aExpression, double& aOutResult, VMWCHAR* aOutError)
{
    if (!aExpression || aExpression[0] == 0)
    {
        aOutResult = 0.0;
        return 0;
    }

    // Convert UCS-2 expression into clean ASCII/Token stream while recognizing Unicode symbols
    Token* tokens = s_tokens;
    int tokenCount = 0;

    int len = wstr_len(aExpression);
    int idx = 0;
    bool mayBeUnary = true;

    while (idx < len && tokenCount < 128)
    {
        VMWCHAR ch = aExpression[idx];

        // Skip spaces
        if (ch == ' ' || ch == '\t')
        {
            idx++;
            continue;
        }

        // Numbers (0-9, '.') with DMS (Degrees, Minutes, Seconds) support
        if ((ch >= '0' && ch <= '9') || ch == '.')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            char numBuf[32];
            int nLen = 0;
            while (idx < len && nLen < 30 && ((aExpression[idx] >= '0' && aExpression[idx] <= '9') || aExpression[idx] == '.'))
            {
                numBuf[nLen++] = (char)aExpression[idx++];
            }
            numBuf[nLen] = 0;

            double v = 0.0;
            vm_sscanf(numBuf, "%lf", &v);

            // Check if this number is the start of a DMS (Degrees ° Minutes ° Seconds °) expression
            if (idx < len && aExpression[idx] == 0x00B0)
            {
                idx++; // consume 1st °
                int dmsParts = 1;
                double dmsVal = v;

                while (true)
                {
                    // Peek if next token is a number followed by °
                    int savedIdx = idx;
                    while (savedIdx < len && (aExpression[savedIdx] == ' ' || aExpression[savedIdx] == '\t')) savedIdx++;

                    if (savedIdx < len && ((aExpression[savedIdx] >= '0' && aExpression[savedIdx] <= '9') || aExpression[savedIdx] == '.'))
                    {
                        int tempIdx = savedIdx;
                        nLen = 0;
                        while (tempIdx < len && nLen < 30 && ((aExpression[tempIdx] >= '0' && aExpression[tempIdx] <= '9') || aExpression[tempIdx] == '.'))
                        {
                            numBuf[nLen++] = (char)aExpression[tempIdx++];
                        }
                        numBuf[nLen] = 0;

                        if (tempIdx < len && aExpression[tempIdx] == 0x00B0)
                        {
                            // It is a continuation of the DMS sequence
                            tempIdx++; // consume °
                            idx = tempIdx;
                            dmsParts++;
                            double subVal = 0.0;
                            vm_sscanf(numBuf, "%lf", &subVal);

                            if (dmsParts == 2)
                            {
                                dmsVal += subVal / 60.0;
                            }
                            else if (dmsParts == 3)
                            {
                                dmsVal += subVal / 3600.0;
                            }
                            else
                            {
                                // More than 3 degrees in sequence -> Syntax Error
                                if (aOutError) wstr_cpy(aOutError, (const VMWCHAR*)u"Syntax Error", KMaxResultLen);
                                return -1;
                            }
                            continue;
                        }
                    }
                    break;
                }

                tokens[tokenCount].type = TOK_NUM;
                tokens[tokenCount].val = dmsVal;
                tokenCount++;
                mayBeUnary = false;
                continue;
            }

            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = v;
            tokenCount++;
            mayBeUnary = false;
            continue;
        }

        // Stray or duplicate degree symbol without preceding number
        if (ch == 0x00B0)
        {
            if (aOutError) wstr_cpy(aOutError, (const VMWCHAR*)u"Syntax Error", KMaxResultLen);
            return -1;
        }

        // Constants: π, e, Ans, PreAns, M
        if (ch == 0x03C0) // π
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = M_PI;
            tokenCount++;
            idx++;
            mayBeUnary = false;
            continue;
        }
        if (ch == 'e' && (idx + 1 >= len || (aExpression[idx+1] < 'a' || aExpression[idx+1] > 'z')))
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = M_E;
            tokenCount++;
            idx++;
            mayBeUnary = false;
            continue;
        }
        if (ch == 'P' && idx + 5 < len && aExpression[idx+1] == 'r' && aExpression[idx+2] == 'e' &&
            aExpression[idx+3] == 'A' && aExpression[idx+4] == 'n' && aExpression[idx+5] == 's')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = iPreAns;
            tokenCount++;
            idx += 6;
            mayBeUnary = false;
            continue;
        }
        if (ch == 'A' && idx + 2 < len && aExpression[idx+1] == 'n' && aExpression[idx+2] == 's')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = iAns;
            tokenCount++;
            idx += 3;
            mayBeUnary = false;
            continue;
        }
        if (ch == 'M' && (idx + 1 >= len || (aExpression[idx+1] != '+' && (aExpression[idx+1] < 'a' || aExpression[idx+1] > 'z'))))
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = iMemoryValue;
            tokenCount++;
            idx++;
            mayBeUnary = false;
            continue;
        }

        // Powers and Superscripts: ², ³, ⁻¹
        if (ch == 0x00B2) // ²
        {
            tokens[tokenCount].type = TOK_OP;
            tokens[tokenCount].op = '^';
            tokens[tokenCount].precedence = 3;
            tokens[tokenCount].rightAssoc = true;
            tokenCount++;

            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = 2.0;
            tokenCount++;
            idx++;
            mayBeUnary = false;
            continue;
        }
        if (ch == 0x00B3) // ³
        {
            tokens[tokenCount].type = TOK_OP;
            tokens[tokenCount].op = '^';
            tokens[tokenCount].precedence = 3;
            tokens[tokenCount].rightAssoc = true;
            tokenCount++;

            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = 3.0;
            tokenCount++;
            idx++;
            mayBeUnary = false;
            continue;
        }
        if (ch == 0x207B && idx + 1 < len && aExpression[idx+1] == 0x00B9) // ⁻¹
        {
            tokens[tokenCount].type = TOK_OP;
            tokens[tokenCount].op = '^';
            tokens[tokenCount].precedence = 3;
            tokens[tokenCount].rightAssoc = true;
            tokenCount++;

            tokens[tokenCount].type = TOK_NUM;
            tokens[tokenCount].val = -1.0;
            tokenCount++;
            idx += 2;
            mayBeUnary = false;
            continue;
        }

        if (ch == 'a' && idx + 2 < len && aExpression[idx+1] == 'b' && aExpression[idx+2] == 's')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "abs");
            tokenCount++;
            idx += 3;
            mayBeUnary = true;
            continue;
        }

        // Functions with implicit multiplication support
        if (ch == 's' && idx + 2 < len && aExpression[idx+1] == 'i' && aExpression[idx+2] == 'n')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "sin");
            tokenCount++;
            idx += 3;
            mayBeUnary = true;
            continue;
        }
        if (ch == 'c' && idx + 2 < len && aExpression[idx+1] == 'o' && aExpression[idx+2] == 's')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "cos");
            tokenCount++;
            idx += 3;
            mayBeUnary = true;
            continue;
        }
        if (ch == 't' && idx + 2 < len && aExpression[idx+1] == 'a' && aExpression[idx+2] == 'n')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "tan");
            tokenCount++;
            idx += 3;
            mayBeUnary = true;
            continue;
        }
        if (ch == 'l' && idx + 1 < len && aExpression[idx+1] == 'n')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "ln");
            tokenCount++;
            idx += 2;
            mayBeUnary = true;
            continue;
        }
        if (ch == 'l' && idx + 2 < len && aExpression[idx+1] == 'o' && aExpression[idx+2] == 'g')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            if (idx + 3 < len && aExpression[idx+3] == 0x2099) // logₙ
            {
                tokens[tokenCount].type = TOK_FUNC;
                strcpy(tokens[tokenCount].func, "logn");
                tokenCount++;
                idx += 4;
            }
            else
            {
                tokens[tokenCount].type = TOK_FUNC;
                strcpy(tokens[tokenCount].func, "log");
                tokenCount++;
                idx += 3;
            }
            mayBeUnary = true;
            continue;
        }
        if (ch == 0x221A) // √ (Unary operator, no parentheses required)
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_OP;
            tokens[tokenCount].op = 's';
            tokens[tokenCount].precedence = 4;
            tokens[tokenCount].rightAssoc = true;
            tokenCount++;
            idx++;
            mayBeUnary = true;
            continue;
        }
        if (ch == 0x221B) // ∛
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "cbrt");
            tokenCount++;
            idx++;
            mayBeUnary = true;
            continue;
        }
        if (ch == 0x207F && idx + 1 < len && aExpression[idx+1] == 0x221A) // ⁿ√
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_FUNC;
            strcpy(tokens[tokenCount].func, "nroot");
            tokenCount++;
            idx += 2;
            mayBeUnary = true;
            continue;
        }

        // Operators & Parentheses
        if (ch == '(')
        {
            CHECK_INSERT_IMPLICIT_MUL();
            tokens[tokenCount].type = TOK_LPAREN;
            tokenCount++;
            idx++;
            mayBeUnary = true;
            continue;
        }
        if (ch == ')')
        {
            tokens[tokenCount].type = TOK_RPAREN;
            tokenCount++;
            idx++;
            mayBeUnary = false;
            continue;
        }
        if (ch == ',')
        {
            tokens[tokenCount].type = TOK_COMMA;
            tokenCount++;
            idx++;
            mayBeUnary = true;
            continue;
        }

        if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '^' || ch == '!' || ch == '%')
        {
            if (ch == '-' && mayBeUnary)
            {
                // Unary minus: 0 - next
                tokens[tokenCount].type = TOK_NUM;
                tokens[tokenCount].val = 0.0;
                tokenCount++;
            }
            tokens[tokenCount].type = TOK_OP;
            tokens[tokenCount].op = (char)ch;
            tokens[tokenCount].precedence = GetPrecedence((char)ch);
            tokens[tokenCount].rightAssoc = (ch == '^');
            tokenCount++;
            idx++;
            mayBeUnary = (ch != '!' && ch != '%');
            continue;
        }

        // Any other character, advance
        idx++;
    }

    // Shunting-Yard to RPN
    Token* rpn = s_rpn;
    int rpnCount = 0;
    Token* opStack = s_opStack;
    int opTop = -1;

    for (int i = 0; i < tokenCount; i++)
    {
        Token& t = tokens[i];
        if (t.type == TOK_NUM)
        {
            rpn[rpnCount++] = t;
        }
        else if (t.type == TOK_FUNC || t.type == TOK_LPAREN)
        {
            opStack[++opTop] = t;
        }
        else if (t.type == TOK_COMMA)
        {
            while (opTop >= 0 && opStack[opTop].type != TOK_LPAREN)
            {
                rpn[rpnCount++] = opStack[opTop--];
            }
        }
        else if (t.type == TOK_OP)
        {
            while (opTop >= 0 && opStack[opTop].type == TOK_OP &&
                   ((!t.rightAssoc && t.precedence <= opStack[opTop].precedence) ||
                    (t.rightAssoc && t.precedence < opStack[opTop].precedence)))
            {
                rpn[rpnCount++] = opStack[opTop--];
            }
            opStack[++opTop] = t;
        }
        else if (t.type == TOK_RPAREN)
        {
            while (opTop >= 0 && opStack[opTop].type != TOK_LPAREN)
            {
                rpn[rpnCount++] = opStack[opTop--];
            }
            if (opTop >= 0 && opStack[opTop].type == TOK_LPAREN)
            {
                opTop--; // pop '('
            }
            if (opTop >= 0 && opStack[opTop].type == TOK_FUNC)
            {
                rpn[rpnCount++] = opStack[opTop--];
            }
            else if (opTop >= 0 && opStack[opTop].type == TOK_OP && opStack[opTop].op == 's')
            {
                rpn[rpnCount++] = opStack[opTop--];
            }
        }
    }

    while (opTop >= 0)
    {
        rpn[rpnCount++] = opStack[opTop--];
    }

    // Evaluate RPN
    double* valStack = s_valStack;
    int valTop = -1;

    for (int i = 0; i < rpnCount; i++)
    {
        Token& t = rpn[i];
        if (t.type == TOK_NUM)
        {
            valStack[++valTop] = t.val;
        }
        else if (t.type == TOK_OP)
        {
            if (t.op == '!')
            {
                if (valTop < 0) return -1;
                double res = Factorial(valStack[valTop]);
                if (res < 0.0) return -3; // Math Error: negative, fractional, or overflow
                valStack[valTop] = res;
            }
            else if (t.op == '%')
            {
                if (valTop < 0) return -1;
                valStack[valTop] = valStack[valTop] / 100.0;
            }
            else if (t.op == 's') // Unary square root: √x
            {
                if (valTop < 0) return -1;
                if (valStack[valTop] < 0.0) return -3; // Math Error
                valStack[valTop] = sqrt(valStack[valTop]);
            }
            else
            {
                if (valTop < 1) return -1;
                double b = valStack[valTop--];
                double a = valStack[valTop--];
                double res = 0.0;
                switch (t.op)
                {
                case '+': res = a + b; break;
                case '-': res = a - b; break;
                case '*': res = a * b; break;
                case '/':
                    if (fabs(b) < 1e-15) return -2; // Division by zero (gag)
                    res = a / b;
                    break;
                case '^':
                    if (a == 0.0 && b < 0.0) return -2; // 0^-x = division by zero
                    if (a < 0.0 && floor(b) != b) return -3; // Math Error: negative base to non-integer power
                    res = pow(a, b);
                    if (isnan(res) || isinf(res)) return -3;
                    break;
                }
                valStack[++valTop] = res;
            }
        }
        else if (t.type == TOK_FUNC)
        {
            if (strcmp(t.func, "sin") == 0)
            {
                if (valTop < 0) return -1;
                double rad = (iAngleMode == EAngleDegrees) ? (valStack[valTop] * M_PI / 180.0) : valStack[valTop];
                valStack[valTop] = sin(rad);
            }
            else if (strcmp(t.func, "cos") == 0)
            {
                if (valTop < 0) return -1;
                double rad = (iAngleMode == EAngleDegrees) ? (valStack[valTop] * M_PI / 180.0) : valStack[valTop];
                valStack[valTop] = cos(rad);
            }
            else if (strcmp(t.func, "tan") == 0)
            {
                if (valTop < 0) return -1;
                if (iAngleMode == EAngleDegrees)
                {
                    double deg = fabs(valStack[valTop]);
                    double rem = fmod(deg, 180.0);
                    if (fabs(rem - 90.0) < 1e-10) return -3; // Math Error: tan(90°), tan(270°), etc.
                    valStack[valTop] = tan(valStack[valTop] * M_PI / 180.0);
                }
                else
                {
                    double rad = fabs(valStack[valTop]);
                    double rem = fmod(rad, M_PI);
                    if (fabs(rem - M_PI / 2.0) < 1e-10) return -3; // Math Error: tan(π/2)
                    valStack[valTop] = tan(valStack[valTop]);
                }
            }
            else if (strcmp(t.func, "log") == 0)
            {
                if (valTop < 0) return -1;
                if (valStack[valTop] <= 0.0) return -3; // Math Error
                valStack[valTop] = log10(valStack[valTop]);
            }
            else if (strcmp(t.func, "ln") == 0)
            {
                if (valTop < 0) return -1;
                if (valStack[valTop] <= 0.0) return -3; // Math Error
                valStack[valTop] = log(valStack[valTop]);
            }
            else if (strcmp(t.func, "sqrt") == 0)
            {
                if (valTop < 0) return -1;
                if (valStack[valTop] < 0.0) return -3; // Math Error
                valStack[valTop] = sqrt(valStack[valTop]);
            }
            else if (strcmp(t.func, "cbrt") == 0)
            {
                if (valTop < 0) return -1;
                valStack[valTop] = cbrt(valStack[valTop]);
            }
            else if (strcmp(t.func, "nroot") == 0) // nroot(n, x) = x^(1/n)
            {
                if (valTop < 1) return -1;
                double x = valStack[valTop--];
                double n = valStack[valTop--];
                if (fabs(n) < 1e-15) return -2;
                if (x < 0.0 && floor(n) == n && ((int)n % 2 == 0)) return -3; // Math Error: even root of negative
                if (x < 0.0)
                    valStack[++valTop] = -pow(-x, 1.0 / n);
                else
                    valStack[++valTop] = pow(x, 1.0 / n);
            }
            else if (strcmp(t.func, "logn") == 0) // logn(base, x) = log(x)/log(base)
            {
                if (valTop < 1) return -1;
                double x = valStack[valTop--];
                double base = valStack[valTop--];
                if (x <= 0.0 || base <= 0.0 || fabs(base - 1.0) < 1e-15) return -3; // Math Error
                valStack[++valTop] = log(x) / log(base);
            }
            else if (strcmp(t.func, "abs") == 0)
            {
                if (valTop < 0) return -1;
                valStack[valTop] = fabs(valStack[valTop]);
            }
        }
    }

    if (valTop == 0)
    {
        if (isnan(valStack[0]) || isinf(valStack[0])) return -3; // Math Error
        aOutResult = valStack[0];
        return 0; // Success
    }

    return -1; // Syntax Error
}
