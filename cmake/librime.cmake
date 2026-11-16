# Static librime 1.17.0 and the libraries it links. Installed under
# build/rime-prefix, which is not committed. No rime.dll is produced.
include(ExternalProject)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

set(VOICEIME_RIME_PREFIX "${CMAKE_BINARY_DIR}/rime-prefix" CACHE PATH "Installed static librime dependencies")
option(VOICEIME_USE_PREBUILT_RIME "Reuse the installed librime prefix without downloading dependencies" OFF)
file(TO_CMAKE_PATH "${VOICEIME_RIME_PREFIX}" VOICEIME_RIME_PREFIX)
set(VOICEIME_RIME_LIBDIR "${VOICEIME_RIME_PREFIX}/lib")
if(VOICEIME_USE_PREBUILT_RIME)
  foreach(required include/rime_api.h lib/librime.a lib/libyaml-cpp.a lib/libleveldb.a lib/libopencc.a lib/libmarisa.a)
    if(NOT EXISTS "${VOICEIME_RIME_PREFIX}/${required}")
      message(FATAL_ERROR "Missing prebuilt Rime dependency: ${VOICEIME_RIME_PREFIX}/${required}")
    endif()
  endforeach()
endif()

set(_voiceime_ep_common
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_INSTALL_PREFIX=${VOICEIME_RIME_PREFIX}
  -DCMAKE_INSTALL_LIBDIR=lib
  -DCMAKE_PREFIX_PATH=${VOICEIME_RIME_PREFIX}
  "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
  "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
  "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DBUILD_SHARED_LIBS=OFF
  "-DCMAKE_CXX_FLAGS=-DNOMINMAX -D_WIN32_WINNT=0x0601")

# Parent CMAKE_BUILD_TYPE is empty. Dependencies are Release so MinGW does not
# look for the yaml-cpp "d" debug postfix. WIN32 static-library search in
# librime's Find modules looks for .lib first, so the archive paths are passed
# in explicitly. Boost is not in the task pin list; librime 1.17 includes
# boost/regex.hpp (header-only in the 1.86 CMake build) plus signals2, unordered,
# algorithm, and range. Boost 1.86.0 is the Boost Software License.
file(WRITE "${CMAKE_BINARY_DIR}/rime-boost-cache.cmake"
"set(BOOST_INCLUDE_LIBRARIES \"algorithm;range;regex;signals2;unordered\" CACHE STRING \"\" FORCE)
set(BOOST_INSTALL_LAYOUT \"system\" CACHE STRING \"\" FORCE)
set(BOOST_ENABLE_PYTHON OFF CACHE BOOL \"\" FORCE)
set(BOOST_ENABLE_MPI OFF CACHE BOOL \"\" FORCE)
set(BUILD_TESTING OFF CACHE BOOL \"\" FORCE)
")

file(WRITE "${CMAKE_BINARY_DIR}/rime-boost-install.cmake"
"execute_process(COMMAND \"${CMAKE_COMMAND}\" --build \"\${BIN}\" --target install RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR \"boost install failed: \${rc}\")
endif()
file(GLOB kids LIST_DIRECTORIES true \"\${SRC}/libs/*\")
foreach(kid \${kids})
  if(IS_DIRECTORY \"\${kid}/include\")
    file(COPY \"\${kid}/include/\" DESTINATION \"\${DEST}/include\")
  endif()
endforeach()
")

# yaml-cpp 0.8.0 does not include <cstdint>. GCC 15+ no longer includes it
# transitively, and pedantic-errors turns that into a hard failure.
file(WRITE "${CMAKE_BINARY_DIR}/rime-patch-yaml.cmake"
[=[
if(NOT EXISTS "${SRC}/src/emitterutils.cpp")
  message(FATAL_ERROR "yaml-cpp emitterutils.cpp missing")
endif()
file(READ "${SRC}/src/emitterutils.cpp" text)
if(NOT text MATCHES "#include <cstdint>")
  string(FIND "${text}" "#include <algorithm>" at)
  if(at LESS 0)
    message(FATAL_ERROR "yaml-cpp emitterutils.cpp has no algorithm include")
  endif()
  string(REPLACE "#include <algorithm>" "#include <algorithm>\n#include <cstdint>" text "${text}")
  file(WRITE "${SRC}/src/emitterutils.cpp" "${text}")
endif()
]=])

file(WRITE "${CMAKE_BINARY_DIR}/marisa-CMakeLists.txt"
[=[
cmake_minimum_required(VERSION 3.16)
project(marisa LANGUAGES CXX)
add_library(marisa STATIC
  lib/marisa/trie.cc
  lib/marisa/agent.cc
  lib/marisa/keyset.cc
  lib/marisa/grimoire/io/reader.cc
  lib/marisa/grimoire/io/writer.cc
  lib/marisa/grimoire/io/mapper.cc
  lib/marisa/grimoire/trie/louds-trie.cc
  lib/marisa/grimoire/trie/tail.cc
  lib/marisa/grimoire/vector/bit-vector.cc)
target_include_directories(marisa
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set_target_properties(marisa PROPERTIES OUTPUT_NAME marisa POSITION_INDEPENDENT_CODE ON)
install(TARGETS marisa ARCHIVE DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)
]=])

# OpenCC's data dictionaries need Python and are unused: voiceime has no
# simplifier. Its CMake links the target name "marisa", which does not exist
# when USE_SYSTEM_MARISA is on.
file(WRITE "${CMAKE_BINARY_DIR}/rime-patch-opencc.cmake"
[=[
if(NOT EXISTS "${SRC}/CMakeLists.txt" OR NOT EXISTS "${SRC}/src/CMakeLists.txt")
  message(FATAL_ERROR "OpenCC sources missing")
endif()
file(READ "${SRC}/CMakeLists.txt" root)
if(root MATCHES "add_subdirectory\\(data\\)")
  string(REPLACE "add_subdirectory(doc)\nadd_subdirectory(data)\n" "" root "${root}")
  if(root MATCHES "add_subdirectory\\(data\\)")
    message(FATAL_ERROR "OpenCC data subdirectory was not removed")
  endif()
  file(WRITE "${SRC}/CMakeLists.txt" "${root}")
endif()
file(READ "${SRC}/src/CMakeLists.txt" src)
if(src MATCHES "target_link_libraries\\(libopencc marisa\\)")
  string(REPLACE "target_link_libraries(libopencc marisa)"
"if(USE_SYSTEM_MARISA)
  target_link_libraries(libopencc \${LIBMARISA})
else()
  target_link_libraries(libopencc marisa)
endif()" src "${src}")
  file(WRITE "${SRC}/src/CMakeLists.txt" "${src}")
endif()
# GCC 15+ does not pull uint16_t in transitively. OpenCC 1.1.9 relies on that.
set(values "${SRC}/src/SerializedValues.hpp")
if(NOT EXISTS "${values}")
  message(FATAL_ERROR "OpenCC SerializedValues.hpp missing")
endif()
file(READ "${values}" header)
if(NOT header MATCHES "#include <cstdint>")
  string(REPLACE "#pragma once" "#pragma once\n\n#include <cstdint>" header "${header}")
  file(WRITE "${values}" "${header}")
endif()
]=])

if(NOT VOICEIME_USE_PREBUILT_RIME)
ExternalProject_Add(voiceime_boost_ext
  URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
  URL_HASH SHA256=2c5ec5edcdff47ff55e27ed9560b0a0b94b07bd07ed9928b476150e16b0efc57
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_voiceime_ep_common}
    -C "${CMAKE_BINARY_DIR}/rime-boost-cache.cmake"
    -DBUILD_TESTING=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} -DSRC=<SOURCE_DIR> -DBIN=<BINARY_DIR> -DDEST=${VOICEIME_RIME_PREFIX}
    -P "${CMAKE_BINARY_DIR}/rime-boost-install.cmake"
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${VOICEIME_RIME_PREFIX}/include/boost/regex.hpp"
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)

ExternalProject_Add(voiceime_yaml_ext
  URL https://github.com/jbeder/yaml-cpp/archive/refs/tags/0.8.0.tar.gz
  URL_HASH SHA256=fbe74bbdcee21d656715688706da3c8becfd946d92cd44705cc6098bb23b3a16
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND ${CMAKE_COMMAND} -DSRC=<SOURCE_DIR> -P "${CMAKE_BINARY_DIR}/rime-patch-yaml.cmake"
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_voiceime_ep_common}
    -DYAML_CPP_BUILD_TESTS=OFF
    -DYAML_CPP_BUILD_TOOLS=OFF
    -DYAML_CPP_BUILD_CONTRIB=OFF
    -DYAML_BUILD_SHARED_LIBS=OFF
    -DYAML_CPP_FORMAT_SOURCE=OFF
    -DBUILD_TESTING=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${VOICEIME_RIME_LIBDIR}/libyaml-cpp.a"
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)

ExternalProject_Add(voiceime_leveldb_ext
  URL https://github.com/google/leveldb/archive/refs/tags/1.23.tar.gz
  URL_HASH SHA256=9a37f8a6174f09bd622bc723b55881dc541cd50747cbd08831c2a82d620f6d76
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_voiceime_ep_common}
    -DLEVELDB_BUILD_TESTS=OFF
    -DLEVELDB_BUILD_BENCHMARKS=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${VOICEIME_RIME_LIBDIR}/libleveldb.a"
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)

ExternalProject_Add(voiceime_marisa_ext
  URL https://github.com/s-yata/marisa-trie/archive/refs/tags/v0.2.6.tar.gz
  URL_HASH SHA256=1063a27c789e75afa2ee6f1716cc6a5486631dcfcb7f4d56d6485d2462e566de
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_BINARY_DIR}/marisa-CMakeLists.txt" <SOURCE_DIR>/CMakeLists.txt
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_voiceime_ep_common}
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${VOICEIME_RIME_LIBDIR}/libmarisa.a"
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)

ExternalProject_Add(voiceime_opencc_ext
  URL https://github.com/BYVoid/OpenCC/archive/refs/tags/ver.1.1.9.tar.gz
  URL_HASH SHA256=ad4bcd8d87219a240a236d4a55c9decd2132a9436697d2882ead85c8939b0a99
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  PATCH_COMMAND ${CMAKE_COMMAND} -DSRC=<SOURCE_DIR> -P "${CMAKE_BINARY_DIR}/rime-patch-opencc.cmake"
  DEPENDS voiceime_marisa_ext
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_voiceime_ep_common}
    "-DCMAKE_CXX_FLAGS=-DNOMINMAX -D_WIN32_WINNT=0x0601 -I${VOICEIME_RIME_PREFIX}/include"
    "-DCMAKE_EXE_LINKER_FLAGS=-L${VOICEIME_RIME_LIBDIR}"
    "-DCMAKE_SHARED_LINKER_FLAGS=-L${VOICEIME_RIME_LIBDIR}"
    -DENABLE_GTEST=OFF
    -DENABLE_BENCHMARK=OFF
    -DBUILD_PYTHON=OFF
    -DBUILD_DOCUMENTATION=OFF
    -DUSE_SYSTEM_MARISA=ON
    "-DLIBMARISA:FILEPATH=${VOICEIME_RIME_LIBDIR}/libmarisa.a"
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${VOICEIME_RIME_LIBDIR}/libopencc.a"
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)

ExternalProject_Add(voiceime_librime_ext
  URL https://github.com/rime/librime/archive/refs/tags/1.17.0.tar.gz
  URL_HASH SHA256=a60274da5d8b8a7187e6c7e9ba5023334ed7bdd182535e93c4e96de8cf188377
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  UPDATE_COMMAND ""
  DEPENDS voiceime_boost_ext voiceime_yaml_ext voiceime_leveldb_ext voiceime_marisa_ext voiceime_opencc_ext
  CMAKE_GENERATOR "${CMAKE_GENERATOR}"
  CMAKE_ARGS ${_voiceime_ep_common}
    "-DCMAKE_CXX_FLAGS=-DNOMINMAX -D_WIN32_WINNT=0x0601 -I${VOICEIME_RIME_PREFIX}/include -DYAML_CPP_STATIC_DEFINE -DOpencc_BUILT_AS_STATIC"
    -DBUILD_TEST=OFF
    -DENABLE_LOGGING=OFF
    -DBUILD_DATA=OFF
    -DENABLE_EXTERNAL_PLUGINS=OFF
    -DBUILD_MERGED_PLUGINS=OFF
    -DBUILD_STATIC=ON
    -DBoost_ROOT=${VOICEIME_RIME_PREFIX}
    -DBoost_USE_STATIC_LIBS=ON
    "-DYamlCpp_LIBRARY:FILEPATH=${VOICEIME_RIME_LIBDIR}/libyaml-cpp.a"
    -DYamlCpp_INCLUDE_PATH:PATH=${VOICEIME_RIME_PREFIX}/include
    "-DLevelDb_LIBRARY:FILEPATH=${VOICEIME_RIME_LIBDIR}/libleveldb.a"
    -DLevelDb_INCLUDE_PATH:PATH=${VOICEIME_RIME_PREFIX}/include
    "-DMarisa_LIBRARY:FILEPATH=${VOICEIME_RIME_LIBDIR}/libmarisa.a"
    -DMarisa_INCLUDE_PATH:PATH=${VOICEIME_RIME_PREFIX}/include
    "-DOpencc_LIBRARY:FILEPATH=${VOICEIME_RIME_LIBDIR}/libopencc.a"
    -DOpencc_INCLUDE_PATH:PATH=${VOICEIME_RIME_PREFIX}/include
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${VOICEIME_RIME_LIBDIR}/librime.a"
  USES_TERMINAL_DOWNLOAD TRUE
  USES_TERMINAL_BUILD TRUE)

endif()

# Static librime contains its own VERSIONINFO resource. Remove only that member
# from a build-local copy so the host DLL can expose Simple Voice's version.
# Never modify a shared/prebuilt prefix used by an installed version.
set(VOICEIME_RIME_CORE "${CMAKE_BINARY_DIR}/rime-core/librime.a")
add_custom_command(OUTPUT "${VOICEIME_RIME_CORE}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/rime-core"
  COMMAND ${CMAKE_COMMAND} -E copy "${VOICEIME_RIME_LIBDIR}/librime.a" "${VOICEIME_RIME_CORE}"
  COMMAND ${CMAKE_AR} d "${VOICEIME_RIME_CORE}" rime.rc.obj
  DEPENDS "${VOICEIME_RIME_LIBDIR}/librime.a"
  VERBATIM)
add_custom_target(voiceime_rime_core DEPENDS "${VOICEIME_RIME_CORE}")
if(NOT VOICEIME_USE_PREBUILT_RIME)
  add_dependencies(voiceime_rime_core voiceime_librime_ext)
endif()

function(voiceime_link_librime target)
  add_dependencies(${target} voiceime_rime_core)
  target_include_directories(${target} PRIVATE "${VOICEIME_RIME_PREFIX}/include")
  target_link_libraries(${target} PRIVATE
    "-Wl,--whole-archive"
    "${VOICEIME_RIME_CORE}"
    "-Wl,--no-whole-archive"
    "${VOICEIME_RIME_LIBDIR}/libyaml-cpp.a"
    "${VOICEIME_RIME_LIBDIR}/libleveldb.a"
    "${VOICEIME_RIME_LIBDIR}/libopencc.a"
    "${VOICEIME_RIME_LIBDIR}/libmarisa.a"
    bcrypt
    ws2_32
    wsock32)
  get_target_property(_voiceime_kind ${target} TYPE)
  if(_voiceime_kind STREQUAL "SHARED_LIBRARY")
    target_link_libraries(${target} PRIVATE "-Wl,-Bstatic" "${VOICEIME_PTHREAD_CORE}" "-Wl,-Bdynamic")
  else()
    target_link_libraries(${target} PRIVATE winpthread)
  endif()
  if(NOT VOICEIME_USE_PREBUILT_RIME)
    add_dependencies(${target}
    voiceime_boost_ext
    voiceime_yaml_ext
    voiceime_leveldb_ext
    voiceime_marisa_ext
    voiceime_opencc_ext
      voiceime_librime_ext)
  endif()
endfunction()
