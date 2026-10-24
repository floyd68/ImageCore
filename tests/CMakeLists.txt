# ImageCore unit tests (enabled with -DIMAGECORE_BUILD_TESTS=ON when configuring
# the FICture2 tree; ImageCore has no standalone CMake project).

add_executable(ImageCoreTests
    TestHarness.h
    TestMain.cpp
    DecodeSchedulerTests.cpp
    ImageLoaderTests.cpp
    LogTests.cpp
)

target_include_directories(ImageCoreTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(ImageCoreTests PRIVATE
    UNICODE
    _UNICODE
)

target_link_libraries(ImageCoreTests PRIVATE
    ImageCore
)

if(MSVC)
    target_compile_options(ImageCoreTests PRIVATE /permissive- /utf-8)
endif()

add_test(NAME ImageCore.DecodeScheduler COMMAND ImageCoreTests DecodeScheduler_)
add_test(NAME ImageCore.Log COMMAND ImageCoreTests Log_)
# ImageLoader is a process singleton whose Shutdown is final: own process.
add_test(NAME ImageCore.ImageLoader COMMAND ImageCoreTests ImageLoader_)
# Safety net only: every wait inside the tests is bounded at 10 s.
set_tests_properties(ImageCore.DecodeScheduler ImageCore.Log ImageCore.ImageLoader PROPERTIES TIMEOUT 120)
