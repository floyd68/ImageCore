add_library(ImageCore STATIC)

target_sources(ImageCore PRIVATE
    DecoderRegistry.cpp
    DecodeScheduler.cpp
    FileByteCache.cpp
    ImageCache.cpp
    ImageCoreLog.cpp
    ImageDecodeDispatcher.cpp
    ImageFormatDetector.cpp
    ImageLoader.cpp
    IPathByteSource.cpp
)

target_include_directories(ImageCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/DirectXTex/DirectXTex
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/DirectXTex/Auxiliary
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/DirectXTex/Common
)

target_compile_definitions(ImageCore PRIVATE
    UNICODE
    _UNICODE
)

target_link_libraries(ImageCore PUBLIC
    DirectXTex
    windowscodecs
    d2d1
)

if(MSVC)
    target_compile_options(ImageCore PRIVATE /permissive-)
endif()

option(IMAGECORE_BUILD_TESTS "Build ImageCore unit tests (configure from the FICture2 tree)" OFF)
if(IMAGECORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
