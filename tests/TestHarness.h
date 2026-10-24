#pragma once

// Minimal ImageCore test harness (no external framework).
//
//   IC_TEST(Suite_Name) { IC_CHECK(x == 1); IC_REQUIRE(ptr != nullptr); }
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
}

#define IC_TEST(name) \
    static void name(); \
    static const ImageCoreTest::Registrar name##_registrar(#name, &name); \
    static void name()

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
