cmake_minimum_required(VERSION 3.20)
project(shaderprep LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHADERPREP_BUILD_TESTS "Build unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- Portable core: builds and is tested on Linux too ---------------------------------------
add_library(shaderprep_core STATIC
  src/core/bytes.cpp
  src/core/hash.cpp
  src/core/pso.cpp
  src/core/psodb.cpp
  src/dxbc/dxbc.cpp
  src/harvest/harvest.cpp
  src/infer/infer.cpp
  src/analyze/analyze.cpp
  src/profile/profile.cpp
)
target_include_directories(shaderprep_core PUBLIC src)
target_include_directories(shaderprep_core PRIVATE third_party/xxhash)
if(MINGW)
  # C99/POSIX printf formats (%zu, %llu) instead of the old msvcrt ones.
  target_compile_definitions(shaderprep_core PUBLIC __USE_MINGW_ANSI_STDIO=1)
endif()
if(WIN32 AND NOT MSVC)
  # MinGW: link the C++ runtime statically so the proxy DLL has no extra dependencies.
  target_link_options(shaderprep_core INTERFACE -static -static-libgcc -static-libstdc++)
endif()

add_executable(shaderprep src/cli/main.cpp)
target_link_libraries(shaderprep PRIVATE shaderprep_core)

# --- Windows-only: D3D12 proxy DLL and out-of-process warm helper ----------------------------
if(WIN32)
  include(FetchContent)
  FetchContent_Declare(DirectX-Headers
    GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
    GIT_TAG v1.619.5
    GIT_SHALLOW TRUE)
  set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(DirectX-Headers)

  add_library(shaderprep_win STATIC
    src/win/bridge.cpp
    src/win/warm.cpp
    src/win/memscan.cpp
    src/win/log.cpp
    src/win/vtable_check.c
  )
  # Note: never link d3d12.lib into anything the proxy DLL uses - it would import itself.
  target_link_libraries(shaderprep_win PUBLIC shaderprep_core DirectX-Headers DirectX-Guids dxguid)
  target_compile_definitions(shaderprep_win PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  if(MINGW)
    # MinGW's rpcndr.h is older than the version DirectX-Headers asks for; the parts we use work.
    target_compile_definitions(shaderprep_win PUBLIC __REQUIRED_RPCNDR_H_VERSION__=475)
  endif()

  # Named d3d12.dll so a game loads it from its own folder; forwards to System32\d3d12.dll.
  add_library(shaderprep_hook SHARED
    src/hook/dllmain.cpp
    src/hook/hooks.cpp
    src/hook/exports.def
  )
  target_link_libraries(shaderprep_hook PRIVATE shaderprep_win)
  set_target_properties(shaderprep_hook PROPERTIES OUTPUT_NAME d3d12 PREFIX "")

  add_executable(shaderprep-warm src/helper/main.cpp)
  target_link_libraries(shaderprep-warm PRIVATE shaderprep_win d3d12 dxgi)
endif()

if(SHADERPREP_BUILD_TESTS)
  enable_testing()
  add_executable(shaderprep_tests
    tests/test_main.cpp
    tests/test_dxbc.cpp
    tests/test_psodb.cpp
    tests/test_harvest.cpp
    tests/test_infer.cpp
    tests/test_profile.cpp
  )
  target_link_libraries(shaderprep_tests PRIVATE shaderprep_core)
  target_include_directories(shaderprep_tests PRIVATE tests)
  target_compile_definitions(shaderprep_tests PRIVATE SHADERPREP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME shaderprep_tests COMMAND shaderprep_tests)
  if(WIN32)
    # Needs a D3D12 device; run next to d3d12.dll (see .github/workflows/ci.yml).
    add_executable(hook_smoke tests/win/hook_smoke.cpp)
    target_link_libraries(hook_smoke PRIVATE shaderprep_win)
  endif()
endif()
