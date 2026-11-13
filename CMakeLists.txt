cmake_minimum_required(VERSION 3.20)
project(VoiceIme LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

# cppjieba yanyiwu/cppjieba cbdf949eadb8887f060473b5dc6d3194655c8bef (MIT).
# Pinyin table: mozillazg/pinyin-data fe29ad32bdf352b31a02b9936e537f575432e9a6 (MIT),
# copied to third_party/cppjieba/dict/pinyin.txt. cppjieba has no pronunciation table.
add_library(voiceime_core STATIC
  src/utf8.cpp
  src/lexicon.cpp
  src/segmenter.cpp
  src/vad.cpp
  src/resample.cpp
  src/session.cpp
  src/place.cpp
  src/ipc.cpp
  third_party/sqlite/sqlite3.c)
target_include_directories(voiceime_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cppjieba/include)
target_compile_definitions(voiceime_core PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
  set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS /w)
else()
  set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS -w)
endif()

add_executable(voiceime_tests tests/main.cpp tests/utf8_test.cpp tests/lexicon_test.cpp tests/segmenter_test.cpp tests/vad_test.cpp tests/session_test.cpp tests/place_test.cpp tests/ipc_test.cpp)
target_link_libraries(voiceime_tests PRIVATE voiceime_core)
target_include_directories(voiceime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
if(MINGW)
  get_filename_component(MINGW_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
  foreach(runtime_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
    add_custom_command(TARGET voiceime_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MINGW_BIN}/${runtime_dll}"
        $<TARGET_FILE_DIR:voiceime_tests>)
  endforeach()
endif()
