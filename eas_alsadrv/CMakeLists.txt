include(CheckSymbolExists)

add_executable(eas_alsadrv eas_alsadrv.c)

find_package(ALSA REQUIRED)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

target_link_libraries(eas_alsadrv ALSA::ALSA sonivox::sonivox Threads::Threads)

list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(secure_getenv stdlib.h HAVE_SECURE_GETENV)
check_symbol_exists(__secure_getenv stdlib.h HAVE___SECURE_GETENV)
list(REMOVE_ITEM CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)

if(HAVE___SECURE_GETENV AND NOT HAVE_SECURE_GETENV)
    target_compile_definitions(eas_alsadrv PRIVATE -Dsecure_getenv=__secure_getenv)
endif()
