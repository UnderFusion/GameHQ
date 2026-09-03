cmake_minimum_required(VERSION 3.24)

# Version comes from the VERSION file (single source of truth, see docs/versioning.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GAMEHQ_VERSION_FULL)
string(STRIP "${GAMEHQ_VERSION_FULL}" GAMEHQ_VERSION_FULL)
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" GAMEHQ_VERSION_BASE "${GAMEHQ_VERSION_FULL}")

# The compact About / What's New modal must remain useful offline. Keep its
# bundled notes tied to VERSION at configure time so a release cannot quietly
# ship the previous build's notes.
set(GAMEHQ_RELEASE_NOTES_FILE "${CMAKE_CURRENT_SOURCE_DIR}/assets/release-notes.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${GAMEHQ_RELEASE_NOTES_FILE}")
file(READ "${GAMEHQ_RELEASE_NOTES_FILE}" GAMEHQ_RELEASE_NOTES_JSON)
string(JSON GAMEHQ_RELEASE_NOTES_VERSION ERROR_VARIABLE GAMEHQ_RELEASE_NOTES_ERROR
    GET "${GAMEHQ_RELEASE_NOTES_JSON}" version)
if(GAMEHQ_RELEASE_NOTES_ERROR OR NOT GAMEHQ_RELEASE_NOTES_VERSION STREQUAL GAMEHQ_VERSION_FULL)
    message(FATAL_ERROR
        "assets/release-notes.json version '${GAMEHQ_RELEASE_NOTES_VERSION}' "
        "must match VERSION '${GAMEHQ_VERSION_FULL}'")
endif()

project(GameHQ VERSION ${GAMEHQ_VERSION_BASE} LANGUAGES C CXX)

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "Disable miniz examples" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "Disable miniz fuzzers" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Disable miniz tests" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "Disable miniz install rules" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
FetchContent_Declare(miniz
    URL https://github.com/richgel999/miniz/archive/refs/tags/3.1.2.tar.gz
    URL_HASH SHA256=98468f8924934b723276680f85238b6c78bf1f8b49b4459cc9b7214a20e2e9fb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(miniz)

# Release-manifest verification uses the reviewed upstream Ed25519
# implementation. Keep this an exact immutable archive pin; 4.0.3 contains
# the June 2026 EdDSA timing-leak fix (docs/release-manifest-security-review.md).
FetchContent_Declare(monocypher
    URL https://monocypher.org/download/monocypher-4.0.3.tar.gz
    URL_HASH SHA512=40904ada5c7ee4f7741733e38b69a30a4b0561cbffba5ffe7c2dce16136d540251ec0d9056ff606510d3b5b708fb8a40db7e0870d4a0b2dc17ba2bfb880f8965
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(monocypher)

# Tests are opt-in so a normal build/run stays as fast as it was; start.bat does
# not enable them. Test builds default to the public RFC 8032 vector, while
# distributable builds default to production trust only.
option(GAMEHQ_BUILD_TESTS "Build the automated test target" OFF)
set(_gamehq_test_key_default OFF)
if(GAMEHQ_BUILD_TESTS)
    set(_gamehq_test_key_default ON)
endif()
option(GAMEHQ_ENABLE_PSEUDO_LOCALES
    "Expose development-only en-XA and ar-XB pseudo-locales"
    OFF)

# Pseudo catalogs and their internal registry exist only in the build tree.
# Production targets never embed or package them unless explicitly opted in.
if(GAMEHQ_BUILD_TESTS OR GAMEHQ_ENABLE_PSEUDO_LOCALES)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(GAMEHQ_PSEUDO_I18N_DIR "${CMAKE_BINARY_DIR}/pseudo-i18n")
    set(GAMEHQ_PSEUDO_MANIFEST "${GAMEHQ_PSEUDO_I18N_DIR}/locales.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/i18n/generate_pseudo.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/i18n/locales.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/i18n/app/gamehq_en_US.ts"
        "${CMAKE_CURRENT_SOURCE_DIR}/i18n/extracted/messages.json")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/i18n/generate_pseudo.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}"
            --output-dir "${GAMEHQ_PSEUDO_I18N_DIR}"
            --manifest-output "${GAMEHQ_PSEUDO_MANIFEST}"
        RESULT_VARIABLE GAMEHQ_PSEUDO_GENERATION_RESULT
        ERROR_VARIABLE GAMEHQ_PSEUDO_GENERATION_ERROR)
    if(NOT GAMEHQ_PSEUDO_GENERATION_RESULT EQUAL 0)
        message(FATAL_ERROR "Pseudo-locale generation failed: ${GAMEHQ_PSEUDO_GENERATION_ERROR}")
    endif()
endif()
option(GAMEHQ_RELEASE_TRUST_TEST_KEYS
    "Compile the TEST-ONLY release signing key into the trust table"
    ${_gamehq_test_key_default})

# The production trust input contains public material only. CMake generates one
# header shared by the app and static updater; private signing material never
# enters the source tree or compiler command line.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/packaging/release-trust.json"
    GAMEHQ_RELEASE_TRUST_JSON)
string(JSON GAMEHQ_RELEASE_TRUST_SCHEMA GET "${GAMEHQ_RELEASE_TRUST_JSON}" schemaVersion)
string(JSON GAMEHQ_PRODUCTION_KEY_ID GET "${GAMEHQ_RELEASE_TRUST_JSON}" currentKey keyId)
string(JSON GAMEHQ_PRODUCTION_PUBLIC_KEY_BASE64 GET
    "${GAMEHQ_RELEASE_TRUST_JSON}" currentKey publicKeyBase64)
string(JSON GAMEHQ_PRODUCTION_KEY_STATE GET
    "${GAMEHQ_RELEASE_TRUST_JSON}" currentKey state)
string(JSON GAMEHQ_PRODUCTION_MINIMUM_RELEASE_SEQUENCE GET
    "${GAMEHQ_RELEASE_TRUST_JSON}" currentKey minimumReleaseSequence)
string(LENGTH "${GAMEHQ_PRODUCTION_KEY_ID}" GAMEHQ_PRODUCTION_KEY_ID_LENGTH)
string(LENGTH "${GAMEHQ_PRODUCTION_PUBLIC_KEY_BASE64}"
    GAMEHQ_PRODUCTION_PUBLIC_KEY_BASE64_LENGTH)
if(NOT GAMEHQ_RELEASE_TRUST_SCHEMA EQUAL 1
   OR NOT GAMEHQ_PRODUCTION_KEY_STATE STREQUAL "current"
   OR GAMEHQ_PRODUCTION_KEY_ID_LENGTH GREATER 64
   OR NOT GAMEHQ_PRODUCTION_KEY_ID MATCHES "^[a-z0-9][a-z0-9._-]*$"
   OR NOT GAMEHQ_PRODUCTION_PUBLIC_KEY_BASE64_LENGTH EQUAL 44
   OR NOT GAMEHQ_PRODUCTION_PUBLIC_KEY_BASE64 MATCHES "^[A-Za-z0-9+/]*=$"
   OR GAMEHQ_PRODUCTION_MINIMUM_RELEASE_SEQUENCE LESS 1)
    message(FATAL_ERROR "packaging/release-trust.json is invalid")
endif()
configure_file(src/security/ReleaseTrustConfig.h.in
    generated/ReleaseTrustConfig.h @ONLY)

add_library(gamehq_release_trust STATIC
    src/security/ReleaseTrust.cpp
    src/security/ReleaseTrust.h
    src/security/ReleaseManifest.cpp
    src/security/ReleaseManifest.h
    "${monocypher_SOURCE_DIR}/src/monocypher.c"
    "${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c")
target_include_directories(gamehq_release_trust PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_BINARY_DIR}/generated"
            "${monocypher_SOURCE_DIR}/src" "${monocypher_SOURCE_DIR}/src/optional")
target_link_libraries(gamehq_release_trust PUBLIC bcrypt)
if(GAMEHQ_RELEASE_TRUST_TEST_KEYS)
    target_compile_definitions(gamehq_release_trust PUBLIC GAMEHQ_RELEASE_TRUST_TEST_KEYS)
endif()

# Brand.qml is the single source of truth for the visible product name. It is
# read at configure time so C++ and QML always expose the same branding.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/qml/Brand.qml" GAMEHQ_BRAND_QML)
string(REGEX MATCH "property string name: \"([^\"]+)\"" _brand_name_match "${GAMEHQ_BRAND_QML}")
set(GAMEHQ_BRAND_NAME "${CMAKE_MATCH_1}")
string(REGEX MATCH "property string slug: \"([^\"]+)\"" _brand_slug_match "${GAMEHQ_BRAND_QML}")
set(GAMEHQ_BRAND_SLUG "${CMAKE_MATCH_1}")
configure_file(src/config/Brand.h.in generated/Brand.h @ONLY)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOMOC_COMPILER_PREDEFINES OFF)

if(WIN32)
    # MinGW's windres is not on PATH (project-local toolchain) — point CMake at
    # the one next to the C++ compiler before enabling the RC language.
    if(NOT CMAKE_RC_COMPILER)
        get_filename_component(_toolchain_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(EXISTS "${_toolchain_bin}/windres.exe")
            # CACHE so the language-determination step (fresh configure) sees it.
            set(CMAKE_RC_COMPILER "${_toolchain_bin}/windres.exe"
                CACHE FILEPATH "windres from the project-local MinGW toolchain")
        endif()
    endif()
    enable_language(RC)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Core Gui Quick Sql Widgets Svg Multimedia Network LinguistTools)

qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0004 NEW)

add_subdirectory(src)

if(GAMEHQ_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
