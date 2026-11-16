# Build the pinned client-compatible interpreter from its vendored sources.
# Its generated VM assembler and layout metadata use the same x86 toolchain.
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(NOT MINGW OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR
    "The pinned LuaJIT source build requires an x86 MinGW Clang or GCC toolchain.")
endif()
get_filename_component(_skeet_luajit_tools "${CMAKE_C_COMPILER}" DIRECTORY)
if(NOT SKEET_LUAJIT_MAKE)
  set(_skeet_luajit_default_make "${_skeet_luajit_tools}/mingw32-make.exe")
  if(EXISTS "${_skeet_luajit_default_make}")
    set(SKEET_LUAJIT_MAKE "${_skeet_luajit_default_make}" CACHE FILEPATH
      "Windows-native GNU make used to build the pinned LuaJIT archive")
  else()
    find_program(SKEET_LUAJIT_MAKE NAMES mingw32-make.exe mingw32-make make.exe
      HINTS "${MSYS2_ROOT}/ucrt64/bin" "${MSYS2_ROOT}/usr/bin"
            "${MSYS2_ROOT}/mingw32/bin")
    if(NOT SKEET_LUAJIT_MAKE)
      message(FATAL_ERROR
        "Set SKEET_LUAJIT_MAKE to a Windows-native GNU make (mingw32-make).")
    endif()
  endif()
endif()
if(NOT SKEET_LUAJIT_AR)
  set(SKEET_LUAJIT_AR "${CMAKE_AR}" CACHE FILEPATH
    "Archiver used to build the pinned LuaJIT archive")
endif()
set(SKEET_LUAJIT_COMPILER_FLAGS "" CACHE STRING
  "Extra compiler flags for the pinned LuaJIT build")
set(_skeet_luajit_compiler_arguments)
foreach(_skeet_luajit_flag IN LISTS SKEET_LUAJIT_COMPILER_FLAGS)
  list(APPEND _skeet_luajit_compiler_arguments "--compiler-flag=${_skeet_luajit_flag}")
endforeach()
set(_skeet_luajit_source "${CMAKE_CURRENT_SOURCE_DIR}/third_party/luajit")
set(_skeet_luajit_build "${CMAKE_CURRENT_BINARY_DIR}/luajit-x86")
set(_skeet_luajit_archive "${_skeet_luajit_build}/src/libluajit.a")
file(GLOB_RECURSE _skeet_luajit_inputs CONFIGURE_DEPENDS "${_skeet_luajit_source}/*")
add_custom_command(OUTPUT "${_skeet_luajit_archive}"
  COMMAND "${Python3_EXECUTABLE}" "${_skeet_luajit_source}/build_x86.py"
    --toolchain "${_skeet_luajit_tools}" --build-dir "${_skeet_luajit_build}"
    --compiler "${CMAKE_C_COMPILER}" ${_skeet_luajit_compiler_arguments}
    --make "${SKEET_LUAJIT_MAKE}" --archiver "${SKEET_LUAJIT_AR}"
  DEPENDS ${_skeet_luajit_inputs}
  COMMENT "Building pinned x86 LuaJIT and its client bytecode-loading rule"
  VERBATIM)
add_custom_target(skeet_luajit_build DEPENDS "${_skeet_luajit_archive}")
add_library(skeet_luajit STATIC IMPORTED GLOBAL)
set_target_properties(skeet_luajit PROPERTIES IMPORTED_LOCATION "${_skeet_luajit_archive}")
add_dependencies(skeet_luajit skeet_luajit_build)
add_dependencies(skeet_core skeet_luajit_build)
target_include_directories(skeet_core PRIVATE "${_skeet_luajit_build}/src")
target_link_libraries(skeet_core PUBLIC skeet_luajit)
