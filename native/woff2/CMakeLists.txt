cmake_minimum_required(VERSION 3.16)
project(woff2_decoder LANGUAGES C CXX)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/woff2/include/woff2/decode.h" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli/c/include/brotli/decode.h")
    set(WOFF2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/woff2")
    set(BROTLI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli")
elseif(EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/woff2_source-src/include/woff2/decode.h" AND
       EXISTS "${CMAKE_CURRENT_BINARY_DIR}/_deps/brotli_source-src/c/include/brotli/decode.h")
    set(WOFF2_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_deps/woff2_source-src")
    set(BROTLI_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_deps/brotli_source-src")
else()
    include(FetchContent)
    FetchContent_Declare(
        woff2_source
        GIT_REPOSITORY https://github.com/google/woff2.git
        GIT_TAG 1bccf208bca986e53a647dfe4811322adb06ecf8
        GIT_SHALLOW TRUE
    )
    FetchContent_Declare(
        brotli_source
        GIT_REPOSITORY https://github.com/google/brotli.git
        GIT_TAG ed738e842d2fbdf2d6459e39267a633c4a9b2f5d
        GIT_SHALLOW TRUE
    )
    FetchContent_GetProperties(woff2_source)
    if(NOT woff2_source_POPULATED)
        FetchContent_Populate(woff2_source)
    endif()
    FetchContent_GetProperties(brotli_source)
    if(NOT brotli_source_POPULATED)
        FetchContent_Populate(brotli_source)
    endif()
    set(WOFF2_ROOT "${woff2_source_SOURCE_DIR}")
    set(BROTLI_ROOT "${brotli_source_SOURCE_DIR}")
endif()

file(GLOB BROTLI_COMMON_SOURCES CONFIGURE_DEPENDS "${BROTLI_ROOT}/c/common/*.c")
file(GLOB BROTLI_DECODER_SOURCES CONFIGURE_DEPENDS "${BROTLI_ROOT}/c/dec/*.c")

add_library(brotlicommon STATIC ${BROTLI_COMMON_SOURCES})
target_include_directories(brotlicommon PUBLIC "${BROTLI_ROOT}/c/include")

add_library(brotlidec STATIC ${BROTLI_DECODER_SOURCES})
target_include_directories(brotlidec PUBLIC "${BROTLI_ROOT}/c/include")
target_link_libraries(brotlidec PUBLIC brotlicommon)

add_library(woff2common STATIC
    "${WOFF2_ROOT}/src/table_tags.cc"
    "${WOFF2_ROOT}/src/variable_length.cc"
    "${WOFF2_ROOT}/src/woff2_common.cc"
)
target_include_directories(woff2common PUBLIC
    "${WOFF2_ROOT}/include"
    "${WOFF2_ROOT}/src"
    "${BROTLI_ROOT}/c/include"
)

add_library(woff2dec STATIC
    "${WOFF2_ROOT}/src/woff2_dec.cc"
    "${WOFF2_ROOT}/src/woff2_out.cc"
)
target_include_directories(woff2dec PUBLIC
    "${WOFF2_ROOT}/include"
    "${WOFF2_ROOT}/src"
    "${BROTLI_ROOT}/c/include"
)
target_link_libraries(woff2dec PUBLIC woff2common brotlidec brotlicommon)
