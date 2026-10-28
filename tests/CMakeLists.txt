# ImageCore unit tests (enabled with -DIMAGECORE_BUILD_TESTS=ON when configuring
# the FICture2 tree; ImageCore has no standalone CMake project).

add_executable(ImageCoreTests
    TestHarness.h
    TestMain.cpp
    DdsMipSelectionTests.cpp
    DecodeSchedulerTests.cpp
    ImageLoaderTests.cpp
    IoLaneTests.cpp
    LogTests.cpp
    SrgbConversionTests.cpp
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
add_test(NAME ImageCore.ReadCancel COMMAND ImageCoreTests ReadCancel_)
add_test(NAME ImageCore.Lanes COMMAND ImageCoreTests Lanes_)
add_test(NAME ImageCore.Log COMMAND ImageCoreTests Log_)
add_test(NAME ImageCore.SrgbConversion COMMAND ImageCoreTests SrgbConversion_)
add_test(NAME ImageCore.DdsMipSelection COMMAND ImageCoreTests DdsMip_)
# ImageLoader is a process singleton whose Shutdown is final: own process.
add_test(NAME ImageCore.ImageLoader COMMAND ImageCoreTests ImageLoader_)
# Safety net only: every wait inside the tests is bounded (10 s; 20 s in the
# lane tests, each also under a 180 s watchdog).
set_tests_properties(ImageCore.DecodeScheduler ImageCore.ReadCancel ImageCore.Log ImageCore.SrgbConversion ImageCore.DdsMipSelection ImageCore.ImageLoader PROPERTIES TIMEOUT 120)
set_tests_properties(ImageCore.Lanes PROPERTIES TIMEOUT 600)
