# nlohmann/json — used to (de)serialize device API request/response bodies.
# Prefer a system package; fall back to FetchContent of the single-header release.
find_package(nlohmann_json 3.10 CONFIG QUIET)

if(NOT nlohmann_json_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()
