# Fetch and build SDL3 (shared). Mirrors the LiveKit SDK example setup so the
# SDL version used for capture/playback matches the one the SDK was tested
# against. SDL3 is not yet in the stable vcpkg/apt baselines, hence FetchContent.
include(FetchContent)

if(NOT TARGET SDL3::SDL3)
  set(SDL_INSTALL OFF CACHE BOOL "Disable SDL3 install" FORCE)
  set(SDL_SHARED  ON  CACHE BOOL "Build shared SDL3"     FORCE)
  set(SDL_STATIC  OFF CACHE BOOL "Skip static SDL3"      FORCE)
  set(SDL_TEST    OFF CACHE BOOL "Skip SDL3 tests"       FORCE)

  # Keep SDL3's own build artifacts out of our lib/ directory.
  set(_save_ar ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
  set(_save_lib ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
  set(_save_run ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  set(_sdl_out ${CMAKE_BINARY_DIR}/_deps/sdl3-build)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${_sdl_out})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${_sdl_out})
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${_sdl_out})

  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.26
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(SDL3)

  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${_save_ar})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${_save_lib})
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${_save_run})
endif()
