// =============================================================================
// KROM Engine - tests/main.cpp
// Test-Runner: ruft alle Test-Suiten auf, gibt Gesamt-Exit-Code zurück.
// =============================================================================
#include "TestFramework.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)
#include <unistd.h>
#endif

// Forward-Deklarationen der Suite-Runner
int RunECSTests();
int RunJobsTests();
int RunRendererTests();
int RunRenderGraphTests();
int RunPlatformTests();
int RunSerializationTests();
int RunAssetsCollisionTests();
int RunEventTests();
int RunECSReadPhaseDeathTest(const char* caseName);

int main(int argc, char** argv)
{
#if defined(_WIN32)
    if (argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0')
        _putenv_s("KROM_TEST_BINARY", argv[0]);
#else
    if (argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0')
        setenv("KROM_TEST_BINARY", argv[0], 1);
#endif

    const char* deathCase = std::getenv("KROM_ECS_DEATH_TEST");
    if (deathCase != nullptr && deathCase[0] != '\0')
        return RunECSReadPhaseDeathTest(deathCase);

    std::fprintf(stdout, "TAP version 14\n");
    std::fprintf(stdout, "# KROM Engine Tests\n");

    int failures = 0;
    failures += RunECSTests();
    failures += RunJobsTests();
    failures += RunRendererTests();
    failures += RunRenderGraphTests();
    failures += RunPlatformTests();
    failures += RunSerializationTests();
    failures += RunAssetsCollisionTests();
    failures += RunEventTests();

    if (failures == 0)
        std::fprintf(stdout, "\n# ALL TESTS PASSED\n");
    else
        std::fprintf(stdout, "\n# FAILURES: %d suite(s) had failures\n", failures);

    (void)argc;
    return failures == 0 ? 0 : 1;
}
