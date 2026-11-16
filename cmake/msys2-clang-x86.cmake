set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)

set(MSYS2_ROOT "$ENV{MSYS2_ROOT}" CACHE PATH
  "MSYS2 installation root containing the ucrt64 clang and mingw32 sysroot")
if(NOT MSYS2_ROOT)
  message(FATAL_ERROR
    "Set MSYS2_ROOT to an MSYS2 installation (for example C:/msys64), either as "
    "an environment variable or with -DMSYS2_ROOT=<path>.")
endif()
file(TO_CMAKE_PATH "${MSYS2_ROOT}" MSYS2_ROOT)
get_filename_component(MSYS2_ROOT "${MSYS2_ROOT}" ABSOLUTE)

set(_skeet_ucrt_bin "${MSYS2_ROOT}/ucrt64/bin")
set(_skeet_mingw_root "${MSYS2_ROOT}/mingw32")
set(CMAKE_C_COMPILER "${_skeet_ucrt_bin}/clang.exe")
set(CMAKE_CXX_COMPILER "${_skeet_ucrt_bin}/clang++.exe")
set(CMAKE_RC_COMPILER "${_skeet_ucrt_bin}/windres.exe")
set(CMAKE_AR "${_skeet_mingw_root}/bin/ar.exe")
set(CMAKE_RANLIB "${_skeet_mingw_root}/bin/ranlib.exe")
set(CMAKE_C_COMPILER_TARGET i686-w64-windows-gnu)
set(CMAKE_CXX_COMPILER_TARGET i686-w64-windows-gnu)

foreach(_skeet_required_tool IN ITEMS CMAKE_C_COMPILER CMAKE_CXX_COMPILER
    CMAKE_RC_COMPILER CMAKE_AR CMAKE_RANLIB)
  if(NOT EXISTS "${${_skeet_required_tool}}")
    message(FATAL_ERROR
      "Missing required x86 tool: ${${_skeet_required_tool}}. "
      "Install the MSYS2 clang and mingw-w64-i686-gcc packages.")
  endif()
endforeach()
foreach(_skeet_required_dir IN ITEMS
    "${_skeet_mingw_root}/include" "${_skeet_mingw_root}/lib")
  if(NOT IS_DIRECTORY "${_skeet_required_dir}")
    message(FATAL_ERROR
      "Missing MSYS2 mingw32 sysroot directory: ${_skeet_required_dir}. "
      "Install the mingw-w64-i686-gcc package.")
  endif()
endforeach()

# GCC's libstdc++ headers unconditionally define NOMINMAX after sources define
# it; the redefinition is harmless and clang's warning would trip -Werror.
set(_skeet_target_flags "--sysroot=${_skeet_mingw_root} -Wno-macro-redefined")
set(CMAKE_C_FLAGS_INIT "${_skeet_target_flags} -m32 -msse2 -mfpmath=sse -ffp-contract=off -fno-fast-math")
set(CMAKE_CXX_FLAGS_INIT "${_skeet_target_flags} -m32 -msse2 -mfpmath=sse -ffp-contract=off -fno-fast-math")
set(SKEET_LUAJIT_COMPILER_FLAGS
  "--target=i686-w64-windows-gnu;--sysroot=${_skeet_mingw_root}" CACHE STRING
  "Extra compiler flags for the pinned LuaJIT build")
set(SKEET_LINKCHECK_COMPILER_FLAGS
  "--target=i686-w64-windows-gnu;--sysroot=${_skeet_mingw_root}" CACHE STRING
  "Target and sysroot flags for the custom link-check command")

list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MSYS2_ROOT)

unset(_skeet_required_tool)
unset(_skeet_required_dir)
unset(_skeet_target_flags)
unset(_skeet_ucrt_bin)
unset(_skeet_mingw_root)
