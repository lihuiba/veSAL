# Copyright (c) 2023 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

include(ExternalProject)

add_library(vesal_third_libs INTERFACE)

target_link_libraries(vesal_third_libs INTERFACE numa)


if(NOT TARGET gflags)
  add_subdirectory(gflags)
endif(TRUE)

target_link_libraries(vesal_third_libs INTERFACE gflags)

if(NOT TARGET spdlog::spdlog)
  add_subdirectory(spdlog)
endif(TRUE)

target_link_libraries(vesal_third_libs INTERFACE spdlog::spdlog)

if(NOT TARGET lz4_static)

  add_subdirectory(lz4/build/cmake)
  target_include_directories(lz4_static INTERFACE lz4/lib)
else()
  # A kind of hack way to include the headers for lz4,
  # because original lz4_static target doesn't contain its include dir.
  get_target_property(lz4_static_source_dir lz4_static SOURCE_DIR)
  set(lz4_static_include "${lz4_static_source_dir}/../../lib")
  target_include_directories(vesal_third_libs INTERFACE ${lz4_static_include})
endif(TRUE)

target_link_libraries(vesal_third_libs INTERFACE lz4_static)

if(${VESAL_ENABLE_ZSTD} AND NOT TARGET libzstd_static)
  set(ZSTD_BUILD_STATIC ON)
  set(ZSTD_BUILD_SHARED OFF)
  add_subdirectory(zstd/build/cmake)
  target_link_libraries(vesal_third_libs INTERFACE libzstd_static)
endif(TRUE)

# introduce tcmalloc from gperftools repo
if(${VESAL_BUILD_PERF})
  project(gperftools)
  include(ExternalProject)
  set(gperftools_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/gperftools)
  set(gperftools_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/gperftools)
  set(minimal_config "--enable-minimal")
  ExternalProject_Add(gperftools
    SOURCE_DIR ${gperftools_SOURCE}
    CONFIGURE_COMMAND autoreconf --force --install -I m4 ${gperftools_SOURCE} &&
    ${gperftools_SOURCE}/configure --prefix=${gperftools_PREFIX}
    --with-pic=yes ${minimal_config}
    BUILD_COMMAND $(MAKE)
    BUILD_IN_SOURCE 0
    INSTALL_COMMAND $(MAKE) install)
  add_library(tcmalloc-static STATIC IMPORTED GLOBAL)
  add_dependencies(tcmalloc-static gperftools)
  set_target_properties(tcmalloc-static PROPERTIES IMPORTED_LOCATION
    ${CMAKE_CURRENT_BINARY_DIR}/gperftools/lib/libtcmalloc_minimal.a)
endif(TRUE)

if(${VESAL_BUILD_TESTS})
  add_subdirectory(googletest)
endif(TRUE)


if(NOT TARGET libdeflate_static)
  # Get the compile options from vesal_compile_options and add them to LIB_COMPILE_OPTIONS
  get_target_property(VESAL_COMPILE_OPTIONS vesal_compile_options INTERFACE_COMPILE_OPTIONS)
  set(LIB_COMPILE_OPTIONS ${LIB_COMPILE_OPTIONS} ${VESAL_COMPILE_OPTIONS})
  add_subdirectory(libdeflate)
  target_link_libraries(vesal_third_libs INTERFACE libdeflate_static)
endif(TRUE)

if(NOT TARGET zlibstatic)
  add_subdirectory(zlib)
  target_link_libraries(vesal_third_libs INTERFACE zlibstatic)
endif(TRUE)

option(USE_SYSTEM_OPENSSL "use system openssl library first" OFF)
if(NOT DEFINED MAKE)
    set(MAKE "/usr/bin/make")
endif(TRUE)

if(${VESAL_ENABLE_SSL})
    if(NOT TARGET ssl-static)
        if(USE_SYSTEM_OPENSSL)
            # https://cmake.org/cmake/help/latest/module/FindOpenSSL.html
            find_package(OpenSSL REQUIRED)

            add_library(ssl-static STATIC IMPORTED GLOBAL)
            set_target_properties(ssl-static PROPERTIES IMPORTED_LOCATION ${OPENSSL_SSL_LIBRARIES})
            target_include_directories(ssl-static INTERFACE ${OPENSSL_INCLUDE_DIR})
            add_library(ssl ALIAS ssl-static)

            add_library(crypto-static STATIC IMPORTED GLOBAL)
            set_target_properties(crypto-static PROPERTIES IMPORTED_LOCATION ${OPENSSL_CRYPTO_LIBRARIES})
            target_include_directories(crypto-static INTERFACE ${OPENSSL_INCLUDE_DIR})
            add_library(crypto ALIAS crypto-static)

        else()
            set(openssl_SOURCE          ${CMAKE_CURRENT_SOURCE_DIR}/openssl)
            set(OPENSSL_ROOT_DIR        ${CMAKE_CURRENT_BINARY_DIR}/openssl CACHE PATH "Path to OpenSSL root directory")
            message("thirdparty openssl root dir: ${OPENSSL_ROOT_DIR}")
            
            # check if openssl has been configured
            if((NOT EXISTS ${openssl_SOURCE}/include) OR (NOT EXISTS ${openssl_SOURCE}/Makefile))
                execute_process(COMMAND ./config -fPIC --prefix=${OPENSSL_ROOT_DIR} 
                                WORKING_DIRECTORY ${openssl_SOURCE}
                                OUTPUT_QUIET)
            else()
                # if prefix is different from the previously configured one, we need to re-config openssl
                file(STRINGS ${openssl_SOURCE}/Makefile configure_args_line REGEX "^CONFIGURE_ARGS=")
                if(configure_args_line)
                    string(REGEX MATCH "--prefix=[^ ]+" prefix_arg ${configure_args_line})
                    string(REGEX REPLACE "--prefix=" "" prefix_value ${prefix_arg})
                    if(NOT ${prefix_value} STREQUAL ${OPENSSL_ROOT_DIR})
                        message("re-config thirdparty openssl, from ${prefix_value} to ${OPENSSL_ROOT_DIR}")
                        execute_process(COMMAND ./config -fPIC --prefix=${OPENSSL_ROOT_DIR} 
                                        WORKING_DIRECTORY ${openssl_SOURCE}
                                        OUTPUT_QUIET)
                    endif(TRUE)
                else()
                    message(FATAL_ERROR "CONFIGURE_ARGS line not found in the config file")
                endif(TRUE)
            endif(TRUE)

            ExternalProject_Add(
                    openssl-project
                    SOURCE_DIR ${openssl_SOURCE}
                    BUILD_IN_SOURCE 1 # building out of source is not supported in v1.0.1
                    CONFIGURE_COMMAND ""
                    BUILD_COMMAND ${MAKE} -j1 # https://github.com/openssl/openssl/issues/5762#issuecomment-376622684
                    INSTALL_COMMAND ${MAKE} install -j1
                    LOG_CONFIGURE ON
                    LOG_BUILD ON
                    LOG_INSTALL ON
            )

            add_library(ssl-static STATIC IMPORTED GLOBAL)
            add_dependencies(ssl-static openssl-project)
            set_target_properties(ssl-static PROPERTIES IMPORTED_LOCATION ${OPENSSL_ROOT_DIR}/lib/libssl.a)
            target_include_directories(ssl-static INTERFACE ${openssl_SOURCE}/include)
            target_link_libraries(ssl-static INTERFACE dl)

            add_library(crypto-static STATIC IMPORTED GLOBAL)
            add_dependencies(crypto-static openssl-project)
            set_target_properties(crypto-static PROPERTIES IMPORTED_LOCATION ${OPENSSL_ROOT_DIR}/lib/libcrypto.a)
            target_include_directories(crypto-static INTERFACE ${openssl_SOURCE}/include)
            target_link_libraries(crypto-static INTERFACE dl)
            # EBS uses crypto but only links to ssl, link crypto to ssl here to avoid compilation issues
            target_link_libraries(ssl-static INTERFACE crypto-static)
            add_library(ssl ALIAS ssl-static)
            add_library(crypto ALIAS crypto-static)
        endif(TRUE)
    endif(TRUE)

    target_link_libraries(vesal_third_libs INTERFACE ssl crypto)
endif(TRUE)

target_include_directories(vesal_third_libs INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/flat_hash_map)
