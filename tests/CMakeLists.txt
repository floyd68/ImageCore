# ImageCore unit tests (enabled with -DIMAGECORE_BUILD_TESTS=ON when configuring
# the FICture2 tree; ImageCore has no standalone CMake project).

add_executable(ImageCoreTests
    TestHarness.h
    TestMain.cpp
    DecodeSchedulerTests.cpp
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
