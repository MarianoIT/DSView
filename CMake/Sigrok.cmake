include(ExternalProject)
find_program(SIGROK_MAKE make REQUIRED)
pkg_check_modules(SIGROK_RUNTIME REQUIRED IMPORTED_TARGET gio-2.0 libusb-1.0)
option(DSVIEW_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)
set(SIGROK_PREFIX "${CMAKE_BINARY_DIR}/sigrok-prefix")
set(SIGROK_SOURCE "${CMAKE_BINARY_DIR}/sigrok-source")
set(SIGROK_BUILD "${CMAKE_BINARY_DIR}/sigrok-build")
set(SIGROK_CFLAGS "-O2 -g")
if(DSVIEW_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    set(SIGROK_CFLAGS "-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer")
endif()
ExternalProject_Add(sigrok_zip_build
    URL "${CMAKE_SOURCE_DIR}/third_party/dist/libzip-1.11.4.tar.gz"
    URL_HASH SHA256=82e9f2f2421f9d7c2466bbc3173cd09595a88ea37db0d559a9d0a2dc60dc722e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${SIGROK_PREFIX}
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_C_FLAGS=${SIGROK_CFLAGS} -DBUILD_SHARED_LIBS=OFF
        -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_EXAMPLES=OFF
        -DBUILD_DOC=OFF -DBUILD_OSSFUZZ=OFF
        -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF
        -DENABLE_OPENSSL=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF
        -DENABLE_COMMONCRYPTO=OFF
    BUILD_BYPRODUCTS "${SIGROK_PREFIX}/lib/libzip.a"
)
ExternalProject_Add(sigrok_upstream_build
    URL "${CMAKE_SOURCE_DIR}/third_party/dist/libsigrok-0.5.2.tar.gz"
    URL_HASH SHA256=4d341f90b6220d3e8cb251dacf726c41165285612248f2c52d15df4590a1ce3c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -DSIGROK_SOURCE=<SOURCE_DIR>
        -P "${CMAKE_SOURCE_DIR}/CMake/PatchSigrok.cmake"
    SOURCE_DIR "${SIGROK_SOURCE}"
    BINARY_DIR "${SIGROK_BUILD}"
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env
        "GIT_CEILING_DIRECTORIES=${CMAKE_BINARY_DIR}"
        "PKG_CONFIG_PATH=${SIGROK_PREFIX}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}"
        "CC=${CMAKE_C_COMPILER}" "CFLAGS=${SIGROK_CFLAGS}"
        <SOURCE_DIR>/configure --prefix=${SIGROK_PREFIX}
        --disable-shared --enable-static --disable-cxx --disable-python
        --disable-ruby --disable-java --disable-all-drivers
        --enable-demo --enable-dreamsourcelab-dslogic
    BUILD_COMMAND ${SIGROK_MAKE} -j4
    INSTALL_COMMAND ${SIGROK_MAKE} install
    BUILD_BYPRODUCTS "${SIGROK_PREFIX}/lib/libsigrok.a"
    DEPENDS sigrok_zip_build
)
file(MAKE_DIRECTORY "${SIGROK_PREFIX}/include")
add_library(dsview_sigrok_core STATIC
    "${CMAKE_SOURCE_DIR}/sigrok/core.c"
)
add_dependencies(dsview_sigrok_core sigrok_upstream_build)
target_include_directories(dsview_sigrok_core BEFORE PRIVATE
    "${SIGROK_BUILD}" "${SIGROK_SOURCE}/src"
    "${SIGROK_PREFIX}/include"
)
target_link_libraries(dsview_sigrok_core PUBLIC
    "${SIGROK_PREFIX}/lib/libsigrok.a"
    "${SIGROK_PREFIX}/lib/libzip.a"
    PkgConfig::SIGROK_RUNTIME ZLIB::ZLIB
)
