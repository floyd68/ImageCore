add_library(ImageCore STATIC)

target_sources(ImageCore PRIVATE
    DecoderRegistry.cpp
    DecodeScheduler.cpp
    FileByteCache.cpp
    ImageCache.cpp
    ImageDecodeDispatcher.cpp
    ImageFormatDetector.cpp
    ImageLoader.cpp
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
