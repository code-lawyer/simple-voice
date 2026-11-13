cmake_minimum_required(VERSION 3.20)
project(VoiceIme LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

add_library(voiceime_core STATIC
  src/utf8.cpp
  src/lexicon.cpp
  third_party/sqlite/sqlite3.c)
target_include_directories(voiceime_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(voiceime_core PRIVATE SQLITE_THREADSAFE=0 SQLITE_OMIT_LOAD_EXTENSION)
if(MSVC)
  set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS /w)
else()
  set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS -w)
endif()

add_executable(voiceime_tests tests/main.cpp tests/utf8_test.cpp tests/lexicon_test.cpp)
target_link_libraries(voiceime_tests PRIVATE voiceime_core)
target_include_directories(voiceime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
