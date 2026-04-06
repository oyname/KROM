#pragma once
// =============================================================================
// KROM Engine - tests/TestFramework.hpp
// Minimales Test-Framework ohne externe Abhängigkeiten.
// Jeder Test ist eine Funktion void(TestContext&).
// Ausgabe: TAP-Format (Test Anything Protocol) - kompatibel mit CTest.
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>
#include <sstream>

namespace test {

struct TestContext
{
    int         passed  = 0;
    int         failed  = 0;
    std::string currentTest;

    void Check(bool condition, const char* expr, const char* file, int line)
    {
        if (condition)
        {
            ++passed;
            std::fprintf(stdout, "  ok %d - %s\n", passed + failed, expr);
        }
        else
        {
            ++failed;
            std::fprintf(stdout, "  not ok %d - %s  # %s:%d\n",
                passed + failed, expr, file, line);
        }
    }

    // Variante für streambare Typen: zeigt beide Werte bei Fehler
    template<typename A, typename B>
    auto CheckEq(const A& a, const B& b, const char* exprA, const char* exprB,
                 const char* file, int line)
        -> std::enable_if_t<std::is_arithmetic_v<A> || std::is_same_v<A, std::string>, void>
    {
        if (a == b) {
            ++passed;
            std::fprintf(stdout, "  ok %d - %s == %s\n", passed + failed, exprA, exprB);
        } else {
            ++failed;
            std::ostringstream ss;
            ss << a << " != " << b;
            std::fprintf(stdout, "  not ok %d - %s == %s  # got: %s  %s:%d\n",
                passed + failed, exprA, exprB, ss.str().c_str(), file, line);
        }
    }

    // Variante für nicht-streambare Typen: zeigt nur Ausdruck
    template<typename A, typename B>
    auto CheckEq(const A& a, const B& b, const char* exprA, const char* exprB,
                 const char* file, int line)
        -> std::enable_if_t<!std::is_arithmetic_v<A> && !std::is_same_v<A, std::string>, void>
    {
        if (a == b) {
            ++passed;
            std::fprintf(stdout, "  ok %d - %s == %s\n", passed + failed, exprA, exprB);
        } else {
            ++failed;
            std::fprintf(stdout, "  not ok %d - %s == %s  # (values not printable)  %s:%d\n",
                passed + failed, exprA, exprB, file, line);
        }
    }
};

// Makros für saubere Ausgabe
#define CHECK(ctx, expr)       (ctx).Check((expr),   #expr,       __FILE__, __LINE__)
#define CHECK_EQ(ctx, a, b)    (ctx).CheckEq((a),(b), #a, #b,     __FILE__, __LINE__)
#define CHECK_NE(ctx, a, b)    (ctx).Check((a)!=(b), #a " != " #b, __FILE__, __LINE__)
#define CHECK_GT(ctx, a, b)    (ctx).Check((a)>(b),  #a " > " #b,  __FILE__, __LINE__)
#define CHECK_GE(ctx, a, b)    (ctx).Check((a)>=(b), #a " >= " #b, __FILE__, __LINE__)
#define CHECK_NULL(ctx, p)     (ctx).Check((p)==nullptr, #p " == nullptr", __FILE__, __LINE__)
#define CHECK_VALID(ctx, h)    (ctx).Check((h).IsValid(), #h ".IsValid()", __FILE__, __LINE__)
#define CHECK_INVALID(ctx, h)  (ctx).Check(!(h).IsValid(), "!" #h ".IsValid()", __FILE__, __LINE__)

struct TestSuite
{
    struct TestCase
    {
        std::string name;
        std::function<void(TestContext&)> fn;
    };

    std::string          suiteName;
    std::vector<TestCase> cases;

    explicit TestSuite(std::string name) : suiteName(std::move(name)) {}

    TestSuite& Add(std::string name, std::function<void(TestContext&)> fn)
    {
        cases.push_back({ std::move(name), std::move(fn) });
        return *this;
    }

    int Run() const
    {
        std::fprintf(stdout, "# Suite: %s\n", suiteName.c_str());
        std::fprintf(stdout, "1..%zu\n", cases.size());

        int suitePassed = 0, suiteFailed = 0;

        for (size_t i = 0; i < cases.size(); ++i)
        {
            const auto& tc = cases[i];
            std::fprintf(stdout, "# Test: %s\n", tc.name.c_str());

            TestContext ctx;
            ctx.currentTest = tc.name;

            try {
                tc.fn(ctx);
            } catch (const std::exception& e) {
                std::fprintf(stdout, "  not ok - EXCEPTION: %s\n", e.what());
                ++ctx.failed;
            } catch (...) {
                std::fprintf(stdout, "  not ok - UNKNOWN EXCEPTION\n");
                ++ctx.failed;
            }

            if (ctx.failed == 0)
            {
                std::fprintf(stdout, "ok %zu - %s (%d checks)\n",
                    i+1, tc.name.c_str(), ctx.passed);
                ++suitePassed;
            }
            else
            {
                std::fprintf(stdout, "not ok %zu - %s (%d passed, %d failed)\n",
                    i+1, tc.name.c_str(), ctx.passed, ctx.failed);
                ++suiteFailed;
            }
        }

        std::fprintf(stdout, "# Suite result: %d passed, %d failed\n",
            suitePassed, suiteFailed);
        return suiteFailed;
    }
};

} // namespace test
