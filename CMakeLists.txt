cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(
  PLUGIN_STANDALONE_XCODE
  "Generate a minimal single-target Xcode project (no install/package/plugin-support targets) for manual signing and notarization via Xcode"
  OFF
)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

# Qt is NOT optional for this plugin, despite the OBS plugin template making it
# so. Everything a streamer needs to get started lives in the Qt sources below:
# account pairing (there is no other way to obtain a device token - the
# dashboard no longer hands one out to paste), the guided model download, the
# Core ML encoder download, and the update check. A build without them cannot be
# connected to an account or given a model, so it is not a usable plugin, and
# the #ifdefs that used to guard all this only ever hid bugs from the two
# configurations anyone actually compiles.
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Network)
if(APPLE)
  # Qt6's WrapOpenGL module unconditionally links "-framework AGL" on
  # macOS. AGL.framework is a genuine, still-present system framework on
  # every currently-shipping macOS (and below our CMAKE_OSX_DEPLOYMENT_TARGET
  # of 12.0), but Apple has dropped it from the SDK bundled with newer Xcode
  # betas, which breaks the link step even though nothing here calls any
  # AGL symbol. cmake/macos/stubs/AGL.framework is a symbol-less stub whose
  # install_name points at the real system path, so it only patches over
  # the SDK's missing framework at build time - the app still binds to the
  # actual system AGL.framework at runtime, same as before Qt. Passed as an
  # explicit link option (rather than CMAKE_FRAMEWORK_PATH) so it lands on
  # this target's own search paths under the Xcode generator too.
  target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-F${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/stubs")
endif()
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/BrandedPopup.cpp
    src/DeviceLinkDialog.cpp
    src/FilterOrigin.cpp
    src/FilterProperties.cpp
    src/GameVocab.cpp
    src/ModelDownloadDialog.cpp
    src/ServerLink.cpp
    src/SharedSettings.cpp
    src/UpdateChecker.cpp
    src/WhisperEngine.cpp
)

# ── Plugin sources ────────────────────────────────────────────────────────────
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE src/TranslatorPlugin.cpp src/WhisperFilter.cpp src/WebSocketClient.cpp src/WhisperWrapper.cpp src/sha256.cpp
)

# ── Require C++17 ─────────────────────────────────────────────────────────────
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_17)

# ── TLS (wss://) via mbedTLS ──────────────────────────────────────────────────
# Same library OBS's own obs-outputs/librtmp.c already uses for RTMPS, and
# already bundled in the OBS deps this plugin builds against - no new
# third-party dependency to fetch. The Windows deps bundle ships a proper
# CMake package config (mirrors how obs-outputs itself finds it); the macOS
# universal bundle only ships the headers/dylibs directly with no config, so
# fall back to a manual find there.
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
find_package(MbedTLS QUIET)
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG FALSE)

if(TARGET MbedTLS::mbedtls)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE MbedTLS::mbedtls)
else()
  find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES include)
  find_library(MBEDTLS_LIB NAMES mbedtls PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib)
  find_library(MBEDX509_LIB NAMES mbedx509 PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib)
  find_library(MBEDCRYPTO_LIB NAMES mbedcrypto PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib)
  if(NOT MBEDTLS_INCLUDE_DIR OR NOT MBEDTLS_LIB OR NOT MBEDX509_LIB OR NOT MBEDCRYPTO_LIB)
    message(
      FATAL_ERROR
      "mbedTLS not found (needed for wss:// support in WebSocketClient.cpp) - "
      "expected headers/libs under one of CMAKE_PREFIX_PATH's entries: ${CMAKE_PREFIX_PATH}"
    )
  endif()
  if(APPLE)
    # MBEDTLS_INCLUDE_DIR is the flattened .deps/include, shared with the Windows
    # VM build and also holding a Windows-only pthread.h/sched.h (needed there,
    # not here). -idirafter keeps mbedtls/ssl.h resolvable while letting the real
    # macOS SDK headers win over the stray Windows ones.
    target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -idirafter ${MBEDTLS_INCLUDE_DIR})
  else()
    target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${MBEDTLS_INCLUDE_DIR})
  endif()
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${MBEDTLS_LIB} ${MBEDX509_LIB} ${MBEDCRYPTO_LIB})
endif()

# ── whisper.cpp ───────────────────────────────────────────────────────────────
if(WIN32)
  set(_whisper_root_default "$ENV{USERPROFILE}/Projects/whisper")
else()
  set(_whisper_root_default "$ENV{HOME}/Projects/whisper")
endif()

set(
  WHISPER_ROOT
  "${_whisper_root_default}"
  CACHE PATH
  "Root of the whisper.cpp checkout.  macOS/Linux: the install prefix \
(contains lib/cmake/whisper/).  Windows: the repo root \
(build/ sub-directory must contain the compiled libs)."
)

if(MSVC)
  # ── Windows: link directly from the build tree - no install required ───────
  # Headers come from the source tree directly.
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${WHISPER_ROOT}/include" "${WHISPER_ROOT}/ggml/include")

  # Per-configuration .lib paths via generator expressions.
  # Each VS configuration picks its own correctly-CRT-matched lib.
  # whisper.lib lives in <build>\src\<config>\
  set(_wb_whisper "${WHISPER_ROOT}/Windows/src")
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      "$<$<CONFIG:Debug>:${_wb_whisper}/Debug/whisper.lib>"
      "$<$<CONFIG:Release>:${_wb_whisper}/Release/whisper.lib>"
      "$<$<CONFIG:RelWithDebInfo>:${_wb_whisper}/RelWithDebInfo/whisper.lib>"
      "$<$<CONFIG:MinSizeRel>:${_wb_whisper}/MinSizeRel/whisper.lib>"
  )
  # ggml libs live in <build>\ggml\src\<config>\
  set(_wb_ggml "${WHISPER_ROOT}/Windows/ggml/src")
  foreach(_lib ggml ggml-base ggml-cpu)
    target_link_libraries(
      ${CMAKE_PROJECT_NAME}
      PRIVATE
        "$<$<CONFIG:Debug>:${_wb_ggml}/Debug/${_lib}.lib>"
        "$<$<CONFIG:Release>:${_wb_ggml}/Release/${_lib}.lib>"
        "$<$<CONFIG:RelWithDebInfo>:${_wb_ggml}/RelWithDebInfo/${_lib}.lib>"
        "$<$<CONFIG:MinSizeRel>:${_wb_ggml}/MinSizeRel/${_lib}.lib>"
    )
  endforeach()

  # Optional GPU backends - set via CMakePresets.json or -DWHISPER_HAS_CUDA=1 etc.
  set(WHISPER_HAS_CUDA 0 CACHE STRING "1 if whisper was built with CUDA")
  set(WHISPER_HAS_HIP 0 CACHE STRING "1 if whisper was built with HIP (ROCm)")
  set(WHISPER_HAS_VULKAN 0 CACHE STRING "1 if whisper was built with Vulkan")

  # ggml's HIP backend is the CUDA backend recompiled with hipcc - it exports the
  # same ggml_backend_cuda_* symbols from ggml-hip.lib instead of ggml-cuda.lib,
  # so the two cannot coexist in one binary.
  if(WHISPER_HAS_CUDA AND WHISPER_HAS_HIP)
    message(
      FATAL_ERROR
      "WHISPER_HAS_CUDA and WHISPER_HAS_HIP are mutually exclusive (both export ggml_backend_cuda_* symbols). Pick one."
    )
  endif()

  if(WHISPER_HAS_CUDA)
    target_link_libraries(
      ${CMAKE_PROJECT_NAME}
      PRIVATE
        "$<$<CONFIG:Debug>:${_wb_ggml}/Debug/ggml-cuda.lib>"
        "$<$<CONFIG:Release>:${_wb_ggml}/Release/ggml-cuda.lib>"
        "$<$<CONFIG:RelWithDebInfo>:${_wb_ggml}/RelWithDebInfo/ggml-cuda.lib>"
        "$<$<CONFIG:MinSizeRel>:${_wb_ggml}/MinSizeRel/ggml-cuda.lib>"
    )
    # CUDA runtime - needed when ggml-cuda.lib is a static library.
    set(
      CUDA_CUDART_LIB
      ""
      CACHE FILEPATH
      "Path to cudart_static.lib. Only needed when building on a machine \
with no local CUDA Toolkit install (e.g. ggml-cuda.lib was cross-built \
elsewhere and copied in) - copy cudart_static.lib from that machine's \
CUDA Toolkit lib\\x64 directory and point this at it."
    )
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE CUDA::cudart_static)
    elseif(CUDA_CUDART_LIB)
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE "${CUDA_CUDART_LIB}")
    else()
      message(
        WARNING
        "WHISPER_HAS_CUDA is set but no CUDA Toolkit was found "
        "and CUDA_CUDART_LIB is not set - the link step will likely fail "
        "with unresolved cuda*/cublas* symbols. Set -DCUDA_CUDART_LIB=<path "
        "to cudart_static.lib> when building without the toolkit installed."
      )
    endif()
  endif()

  if(WHISPER_HAS_HIP)
    set(_wb_ggml_hip "${WHISPER_ROOT}/Windows/ggml/src/ggml-hip")
    target_link_libraries(
      ${CMAKE_PROJECT_NAME}
      PRIVATE
        "$<$<CONFIG:Debug>:${_wb_ggml_hip}/Debug/ggml-hip.lib>"
        "$<$<CONFIG:Release>:${_wb_ggml_hip}/Release/ggml-hip.lib>"
        "$<$<CONFIG:RelWithDebInfo>:${_wb_ggml_hip}/RelWithDebInfo/ggml-hip.lib>"
        "$<$<CONFIG:MinSizeRel>:${_wb_ggml_hip}/MinSizeRel/ggml-hip.lib>"
    )
    # HIP runtime (ROCm) - needed when ggml-hip.lib is a static library
    find_package(hip QUIET)
    if(hip_FOUND)
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE hip::host)
    else()
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE amdhip64)
    endif()
  endif()

  if(WHISPER_HAS_VULKAN)
    set(_wb_ggml_vk "${WHISPER_ROOT}/Windows/ggml/src/ggml-vulkan")
    target_link_libraries(
      ${CMAKE_PROJECT_NAME}
      PRIVATE
        "$<$<CONFIG:Debug>:${_wb_ggml_vk}/Debug/ggml-vulkan.lib>"
        "$<$<CONFIG:Release>:${_wb_ggml_vk}/Release/ggml-vulkan.lib>"
        "$<$<CONFIG:RelWithDebInfo>:${_wb_ggml_vk}/RelWithDebInfo/ggml-vulkan.lib>"
        "$<$<CONFIG:MinSizeRel>:${_wb_ggml_vk}/MinSizeRel/ggml-vulkan.lib>"
    )
    # Vulkan loader
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Vulkan::Vulkan)
    else()
      target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE vulkan-1)
    endif()
  endif()

  target_compile_definitions(
    ${CMAKE_PROJECT_NAME}
    PRIVATE
      HAS_GGML_CUDA=${WHISPER_HAS_CUDA}
      HAS_GGML_HIP=${WHISPER_HAS_HIP}
      HAS_GGML_VULKAN=${WHISPER_HAS_VULKAN}
      HAS_WHISPER_COREML=0 # Core ML / ANE is macOS-only; never available on Windows
  )
else()
  # ── macOS / Linux: use cmake packages from the install prefix ─────────────
  set(_whisper_prefix "${WHISPER_ROOT}")

  # whisper-config.cmake calls find_package(ggml) using LLAMA_LIB_DIR (a
  # copy-paste bug from llama.cpp), so we must point ggml_DIR ourselves.
  set(ggml_DIR "${_whisper_prefix}/lib/cmake/ggml")
  find_package(whisper REQUIRED HINTS "${_whisper_prefix}/lib/cmake/whisper")

  # Use cmake variables exported by ggml-config.cmake for backend detection.
  if(GGML_CUDA)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_GGML_CUDA=1)
  else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_GGML_CUDA=0)
  endif()

  if(GGML_HIP)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_GGML_HIP=1)
  else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_GGML_HIP=0)
  endif()

  if(GGML_VULKAN)
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_GGML_VULKAN=1)
  else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_GGML_VULKAN=0)
  endif()

  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE whisper)

  # whisper.cpp's installed whisper-config.cmake does NOT export the Core ML
  # encoder sublibrary or its frameworks (no whisper.coreml target, empty
  # framework list - verified on 1.8.4). So a WHISPER_COREML build leaves
  # libwhisper.a with undefined _whisper_coreml_{init,encode,free} and
  # _OBJC_CLASS_$_MLModel symbols that find_package(whisper) alone won't
  # satisfy. Link the archive + its frameworks ourselves, macOS-only, and only
  # when the archive is actually present (a non-Core ML whisper build, or Linux,
  # falls straight through to HAS_WHISPER_COREML=0). Foundation + CoreML are the
  # frameworks src/coreml/whisper-encoder.mm references.
  set(_whisper_coreml_lib "${_whisper_prefix}/lib/libwhisper.coreml.a")
  if(APPLE AND EXISTS "${_whisper_coreml_lib}")
    target_link_libraries(
      ${CMAKE_PROJECT_NAME}
      PRIVATE "${_whisper_coreml_lib}" "-framework CoreML" "-framework Foundation"
    )
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_WHISPER_COREML=1)
    message(STATUS "[babelstreamer] Core ML encoder enabled (${_whisper_coreml_lib})")
  else()
    target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE HAS_WHISPER_COREML=0)
  endif()
endif()

# Windows needs ws2_32 for BSD sockets used by WebSocketClient.cpp,
# advapi32 for the registry read (MachineGuid) in WhisperFilter.cpp's
# computeDeviceFingerprint() - free-trial anti-abuse - and crypt32 for
# CertOpenSystemStore/CertEnumCertificatesInStore (WebSocketClient.cpp's
# loadSystemRootCerts, wss:// certificate verification). None of these are
# linked implicitly here the way they might be via a default MSVC toolchain
# setup, since this file already links ws2_32 explicitly rather than relying
# on defaults, so they're all added explicitly.
if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32 advapi32 crypt32)
endif()

# macOS: IOKit for the IOPlatformUUID read in computeDeviceFingerprint()
# (WhisperFilter.cpp) - free-trial anti-abuse. Security for
# SecTrustCopyAnchorCertificates/SecCertificateCopyData (WebSocketClient.cpp's
# loadSystemRootCerts, wss:// certificate verification) - CoreFoundation is
# linked alongside it since that's where the CFArrayRef/CFDataRef types those
# calls use come from; verify this resolves cleanly on both the Xcode-generator
# build and the macos-ci preset before relying on it.
if(APPLE)
  target_link_libraries(
    ${CMAKE_PROJECT_NAME}
    PRIVATE "-framework IOKit" "-framework Security" "-framework CoreFoundation"
  )
endif()

# ── OBS plugin packaging ──────────────────────────────────────────────────────
# Wraps the .so into a .plugin bundle on macOS, sets the Xcode install path to
# ~/Library/Application Support/obs-studio/plugins, and wires up the post-build
# copy to build_macos/rundir/<Config>/ for quick manual installs.
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES ${_plugin_properties})

# SKIP_INSTALL defaults to YES for bundle targets in Xcode, which causes Archive
# to produce an empty Products folder.  Set it to NO so the .plugin bundle is
# placed into the archive and shows up in the Xcode Organizer.
if(XCODE)
  set_property(TARGET ${CMAKE_PROJECT_NAME} PROPERTY XCODE_ATTRIBUTE_SKIP_INSTALL "NO")
endif()
