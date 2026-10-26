#include "TestHarness.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

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

    std::vector<ChildCase>& ChildRegistry()
    {
        static std::vector<ChildCase> registry;
        return registry;
    }

    unsigned long RunChild(const char* name, unsigned long timeoutMs)
    {
        wchar_t exe[MAX_PATH] {};
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH) == 0)
        {
            return static_cast<unsigned long>(-1);
        }
        std::wstring commandLine = L"\"" + std::wstring(exe) + L"\" --child ";
        for (const char* c = name; *c != '\0'; ++c)
        {
            commandLine.push_back(static_cast<wchar_t>(*c));
        }

        STARTUPINFOW startup { sizeof(startup) };
        PROCESS_INFORMATION process {};
        if (!CreateProcessW(exe, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        {
            return static_cast<unsigned long>(-1);
        }
        DWORD exitCode = STILL_ACTIVE;
        if (WaitForSingleObject(process.hProcess, timeoutMs) == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(process.hProcess, &exitCode);
        }
        else
        {
            TerminateProcess(process.hProcess, 1);
            exitCode = STILL_ACTIVE;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return exitCode;
    }
}

// Usage: ImageCoreTests [--list | <name prefix> | --child <name> [args...]]
int main(int argc, char** argv)
{
    if (argc > 2 && std::strcmp(argv[1], "--child") == 0)
    {
        for (const auto& child : ImageCoreTest::ChildRegistry())
        {
            if (std::strcmp(child.name, argv[2]) == 0)
            {
                return child.fn(argc - 3, argv + 3);
            }
        }
        std::fprintf(stderr, "unknown child: %s\n", argv[2]);
        return 2;
    }

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
