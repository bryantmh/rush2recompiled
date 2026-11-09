# Makes find_package(zstd CONFIG) find rt64's zstd (libzstd_static), for libchdr (CMakeLists.txt, "CHD disc images").
if(NOT TARGET zstd::zstd)
    add_library(zstd::zstd ALIAS libzstd_static)
endif()
set(zstd_FOUND TRUE)
