cmake_minimum_required(VERSION 3.20)
project(VoiceIme LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/utf-8)
endif()

add_library(voiceime_core STATIC src/utf8.cpp)
target_include_directories(voiceime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(voiceime_tests tests/main.cpp tests/utf8_test.cpp)
target_link_libraries(voiceime_tests PRIVATE voiceime_core)
target_include_directories(voiceime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
