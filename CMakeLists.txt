cmake_minimum_required(VERSION 3.13)
project(eas_alsadrv_xxx)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BUILD_SONIVOX_SHARED ON)
set(BUILD_SONIVOX_STATIC OFF)
set(BUILD_EXAMPLE OFF)
set(BUILD_TESTING OFF)

add_subdirectory(sonivox)
add_subdirectory(eas_alsadrv)

