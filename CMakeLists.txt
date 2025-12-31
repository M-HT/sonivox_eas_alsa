cmake_minimum_required(VERSION 3.13)
project(eas_alsadrv_xxx)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SHARED_LIBS ON)
set(SF2_SUPPORT OFF)
set(ZLIB_SUPPORT OFF)
set(BUILD_TESTING OFF)
set(BUILD_APPLICATION OFF)
set(BUILD_MANPAGE OFF)

add_subdirectory(sonivox)
add_subdirectory(eas_alsadrv)

