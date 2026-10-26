#pragma once

// Minimal ImageCore test harness (no external framework).
//
//   IC_TEST(Suite_Name) { IC_CHECK(x == 1); IC_REQUIRE(ptr != nullptr); }
//   IC_CHILD(Name) { return 0; }  // `ImageCoreTests --child Name [args...]`
//
// IC_CHECK records a failure and continues; IC_REQUIRE records a failure and
// aborts the current test. TestMain.cpp runs every test whose name starts with
// the optional command-line prefix and returns non-zero on any failure.

#include <functional>
#include <vector>

namespace ImageCoreTest
{
    using TestFn = void (*)();

    struct TestCase
    {
        const char* name { nullptr };
        TestFn fn { nullptr };
    };

    std::vector<TestCase>& Registry();

    struct Registrar
    {
        Registrar(const char* name, TestFn fn)
        {
            Registry().push_back({ name, fn });
        }
    };

    struct RequireFailed
    {
    };

    void ReportFailure(const char* file, int line, const char* expression);

    // Runs once after all selected tests.
    void RegisterTeardown(std::function<void()> teardown);

    // Child-process entry points, for checks that need a fresh process (for
    // example one in which a process singleton was never created):
    // `ImageCoreTests --child <name> [args...]` runs one and exits with its
    // result. IC_CHECK/IC_REQUIRE are not used there; return a non-zero code.
    using ChildFn = int (*)(int argc, char** argv);  // the args after <name>

    struct ChildCase
    {
        const char* name { nullptr };
        ChildFn fn { nullptr };
    };

    std::vector<ChildCase>& ChildRegistry();

    struct ChildRegistrar
    {
        ChildRegistrar(const char* name, ChildFn fn)
        {
            ChildRegistry().push_back({ name, fn });
        }
    };

    // Runs `ImageCoreTests --child <name>` and returns its exit code, or
    // STILL_ACTIVE (after terminating it) if it does not exit within `timeoutMs`.
    unsigned long RunChild(const char* name, unsigned long timeoutMs);
}

#define IC_TEST(name) \
    static void name(); \
    static const ImageCoreTest::Registrar name##_registrar(#name, &name); \
    static void name()

#define IC_CHILD(name) \
    static int name(int argc, char** argv); \
    static const ImageCoreTest::ChildRegistrar name##_registrar(#name, &name); \
    static int name(int argc, char** argv)

#define IC_CHECK(expr) \
    do { \
        if (!(expr)) \
        { \
            ImageCoreTest::ReportFailure(__FILE__, __LINE__, #expr); \
        } \
    } while (0)

#define IC_REQUIRE(expr) \
    do { \
        if (!(expr)) \
        { \
            ImageCoreTest::ReportFailure(__FILE__, __LINE__, #expr); \
            throw ImageCoreTest::RequireFailed {}; \
        } \
    } while (0)
