# 64-bit Windows via MinGW-w64. This triplet uses msvcrt, same as Rust's
# x86_64-pc-windows-gnu target.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(_nlink_mingw "/usr/x86_64-w64-mingw32")
if(EXISTS "${_nlink_mingw}/sys-root/mingw/include")
  set(_nlink_mingw_root "${_nlink_mingw}/sys-root/mingw")
else()
  set(_nlink_mingw_root "${_nlink_mingw}")
endif()

set(CMAKE_FIND_ROOT_PATH "${_nlink_mingw_root}")
set(CMAKE_PREFIX_PATH "${_nlink_mingw_root}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(EXISTS "${_nlink_mingw_root}/lib/cmake/Qt6/Qt6Config.cmake")
  set(Qt6_DIR "${_nlink_mingw_root}/lib/cmake/Qt6" CACHE PATH "MinGW Qt6 cmake package" FORCE)
endif()
set(NLINK_CARGO_TARGET "x86_64-pc-windows-gnu" CACHE STRING "Rust target triple for the Evo staticlib" FORCE)
