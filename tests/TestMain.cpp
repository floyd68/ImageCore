#include "TestHarness.h"

#include <cstdio>
#include <cstring>
#include <exception>

namespace
{
    int g_failuresInCurrentTest = 0;
    int g_totalFailures = 0;

    std::vector<std::function<void()>>& Teardowns()
    {
        static std::vector<std::function<void()>> teardowns;
        return teardowns;
    }
}

namespace ImageCoreTest
{
    std::vector<TestCase>& Registry()
    {
        static std::vector<TestCase> registry;
        return registry;
    }

    void ReportFailure(const char* file, int line, const char* expression)
    {
        ++g_failuresInCurrentTest;
        ++g_totalFailures;
        std::fprintf(stderr, "  %s(%d): check failed: %s\n", file, line, expression);
    }

    void RegisterTeardown(std::function<void()> teardown)
    {
        Teardowns().push_back(std::move(teardown));
    }
}

// Usage: ImageCoreTests [--list | <name prefix>]
int main(int argc, char** argv)
{
    const char* prefix = (argc > 1) ? argv[1] : "";
    if (std::strcmp(prefix, "--list") == 0)
    {
        for (const auto& test : ImageCoreTest::Registry())
        {
            std::printf("%s\n", test.name);
        }
        return 0;
    }

    int run = 0;
    int failedTests = 0;
    for (const auto& test : ImageCoreTest::Registry())
    {
        if (std::strncmp(test.name, prefix, std::strlen(prefix)) != 0)
        {
            continue;
        }

        ++run;
        g_failuresInCurrentTest = 0;
        std::printf("[ RUN  ] %s\n", test.name);
        try
        {
            test.fn();
        }
        catch (const ImageCoreTest::RequireFailed&)
        {
        }
        catch (const std::exception& ex)
        {
            ImageCoreTest::ReportFailure(__FILE__, __LINE__, ex.what());
        }
        catch (...)
        {
            ImageCoreTest::ReportFailure(__FILE__, __LINE__, "unknown exception");
        }

        if (g_failuresInCurrentTest == 0)
        {
            std::printf("[  OK  ] %s\n", test.name);
        }
        else
        {
            ++failedTests;
            std::printf("[ FAIL ] %s\n", test.name);
        }
    }

    for (const auto& teardown : Teardowns())
    {
        teardown();
    }

    std::printf("%d test(s) run, %d failed\n", run, failedTests);
    if (run == 0)
    {
        std::fprintf(stderr, "no tests match prefix '%s'\n", prefix);
        return 1;
    }
    return (g_totalFailures == 0) ? 0 : 1;
}
