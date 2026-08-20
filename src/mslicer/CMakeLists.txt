set(TIGERSLICER_MSLICER_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/mslicer")
set(TIGERSLICER_MSLICER_MANIFEST "${TIGERSLICER_MSLICER_VENDOR_DIR}/Cargo.toml")

set(_cargo_hints)
if(DEFINED ENV{CARGO_HOME})
    list(APPEND _cargo_hints "$ENV{CARGO_HOME}/bin")
endif()
list(APPEND _cargo_hints "C:/src/TigerSlicer-deps/rust/cargo/bin")
find_program(TIGERSLICER_CARGO_EXECUTABLE NAMES cargo HINTS ${_cargo_hints})
if(NOT TIGERSLICER_CARGO_EXECUTABLE)
    message(FATAL_ERROR
        "Cargo is required for the integrated MSlicer engine. "
        "Install the documented Rust toolchain or set CARGO_HOME.")
endif()

file(GLOB_RECURSE TIGERSLICER_MSLICER_RUST_SOURCES CONFIGURE_DEPENDS
    "${TIGERSLICER_MSLICER_VENDOR_DIR}/common/src/*.rs"
    "${TIGERSLICER_MSLICER_VENDOR_DIR}/format/*/src/*.rs"
    "${TIGERSLICER_MSLICER_VENDOR_DIR}/remote_print/src/*.rs"
    "${TIGERSLICER_MSLICER_VENDOR_DIR}/remote_print/src/**/*.rs"
    "${TIGERSLICER_MSLICER_VENDOR_DIR}/slicer/src/*.rs"
    "${TIGERSLICER_MSLICER_VENDOR_DIR}/tigerslicer_bridge/src/*.rs")

set(TIGERSLICER_MSLICER_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(TIGERSLICER_MSLICER_CARGO_ENV
    "CARGO_TARGET_DIR=${TIGERSLICER_MSLICER_TARGET_DIR}"
    "CARGO_NET_OFFLINE=true")
if(DEFINED ENV{CARGO_HOME})
    list(APPEND TIGERSLICER_MSLICER_CARGO_ENV "CARGO_HOME=$ENV{CARGO_HOME}")
endif()
if(DEFINED ENV{RUSTUP_HOME})
    list(APPEND TIGERSLICER_MSLICER_CARGO_ENV "RUSTUP_HOME=$ENV{RUSTUP_HOME}")
elseif(EXISTS "C:/src/TigerSlicer-deps/rust/rustup")
    list(APPEND TIGERSLICER_MSLICER_CARGO_ENV
        "RUSTUP_HOME=C:/src/TigerSlicer-deps/rust/rustup"
        "CARGO_HOME=C:/src/TigerSlicer-deps/rust/cargo")
endif()
set(TIGERSLICER_MSLICER_DEBUG_LIB
    "${TIGERSLICER_MSLICER_TARGET_DIR}/debug/${CMAKE_STATIC_LIBRARY_PREFIX}tigerslicer_mslicer_bridge${CMAKE_STATIC_LIBRARY_SUFFIX}")
set(TIGERSLICER_MSLICER_RELEASE_LIB
    "${TIGERSLICER_MSLICER_TARGET_DIR}/release/${CMAKE_STATIC_LIBRARY_PREFIX}tigerslicer_mslicer_bridge${CMAKE_STATIC_LIBRARY_SUFFIX}")

add_custom_target(tigerslicer_mslicer_rust ALL
    COMMAND ${CMAKE_COMMAND} -E env
        ${TIGERSLICER_MSLICER_CARGO_ENV}
        "${TIGERSLICER_CARGO_EXECUTABLE}" build --locked --offline
        --manifest-path "${TIGERSLICER_MSLICER_MANIFEST}"
        -p tigerslicer_mslicer_bridge
        $<$<NOT:$<CONFIG:Debug>>:--release>
    BYPRODUCTS
        "${TIGERSLICER_MSLICER_DEBUG_LIB}"
        "${TIGERSLICER_MSLICER_RELEASE_LIB}"
    DEPENDS
        ${TIGERSLICER_MSLICER_RUST_SOURCES}
        "${TIGERSLICER_MSLICER_VENDOR_DIR}/Cargo.toml"
        "${TIGERSLICER_MSLICER_VENDOR_DIR}/Cargo.lock"
        "${TIGERSLICER_MSLICER_VENDOR_DIR}/tigerslicer_bridge/Cargo.toml"
    COMMENT "Building pinned in-process MSlicer Rust engine"
    VERBATIM)

add_library(tigerslicer_mslicer_rust_library STATIC IMPORTED GLOBAL)
set_target_properties(tigerslicer_mslicer_rust_library PROPERTIES
    IMPORTED_LOCATION_DEBUG "${TIGERSLICER_MSLICER_DEBUG_LIB}"
    IMPORTED_LOCATION_RELEASE "${TIGERSLICER_MSLICER_RELEASE_LIB}"
    IMPORTED_LOCATION_RELWITHDEBINFO "${TIGERSLICER_MSLICER_RELEASE_LIB}"
    IMPORTED_LOCATION_MINSIZEREL "${TIGERSLICER_MSLICER_RELEASE_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_dependencies(tigerslicer_mslicer_rust_library tigerslicer_mslicer_rust)

if(WIN32)
    target_link_libraries(tigerslicer_mslicer_rust_library INTERFACE
        advapi32 bcrypt kernel32 ntdll userenv ws2_32)
elseif(UNIX)
    target_link_libraries(tigerslicer_mslicer_rust_library INTERFACE
        ${CMAKE_DL_LIBS} pthread m)
endif()

add_library(tigerslicer_mslicer_cpp STATIC
    cpp/MSlicerBridge.cpp
    cpp/MSlicerBridge.hpp
    include/tigerslicer_mslicer.h)
target_include_directories(tigerslicer_mslicer_cpp PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(tigerslicer_mslicer_cpp PUBLIC tigerslicer_mslicer_rust_library)

if(SLIC3R_BUILD_TESTS)
    add_executable(tigerslicer_mslicer_bridge_tests cpp/MSlicerBridgeTests.cpp)
    target_link_libraries(tigerslicer_mslicer_bridge_tests PRIVATE tigerslicer_mslicer_cpp)
    add_test(NAME tigerslicer_mslicer_bridge COMMAND tigerslicer_mslicer_bridge_tests)
endif()
