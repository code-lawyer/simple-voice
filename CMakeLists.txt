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
  src/settings.cpp
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

add_executable(voiceime_tests tests/main.cpp tests/utf8_test.cpp tests/lexicon_test.cpp tests/segmenter_test.cpp tests/vad_test.cpp tests/session_test.cpp tests/place_test.cpp tests/ipc_test.cpp tests/tsf_load_test.cpp tests/settings_test.cpp)
target_link_libraries(voiceime_tests PRIVATE voiceime_core)
target_include_directories(voiceime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(voiceime_engine src/engine_main.cpp src/confirm_box.cpp src/sherpa_recognizer.cpp)
target_link_libraries(voiceime_engine PRIVATE voiceime_core ole32 uuid user32 gdi32 shell32)
target_compile_definitions(voiceime_engine PRIVATE _WIN32_WINNT=0x0601)
include(cmake/sherpa.cmake)
voiceime_use_sherpa(voiceime_engine)

function(voiceime_copy_runtime target)
  if(MINGW)
    get_filename_component(MINGW_BIN ${CMAKE_CXX_COMPILER} DIRECTORY)
    foreach(runtime_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${MINGW_BIN}/${runtime_dll}"
          $<TARGET_FILE_DIR:${target}>)
    endforeach()
  endif()
endfunction()
voiceime_copy_runtime(voiceime_tests)
voiceime_copy_runtime(voiceime_engine)

# The text service is loaded into other processes. Those hosts do not search
# next to this DLL for libstdc++, so the runtime is linked in.
add_library(voiceime_tsf SHARED src/tsf_main.cpp src/tsf_service.cpp src/ipc.cpp)
set_target_properties(voiceime_tsf PROPERTIES PREFIX "" OUTPUT_NAME "voiceime_tsf")
target_link_libraries(voiceime_tsf PRIVATE ole32 oleaut32 uuid user32 gdi32 "-Wl,-Bstatic" "-Wl,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive" "-Wl,-Bdynamic")
target_include_directories(voiceime_tsf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(voiceime_tsf PRIVATE _WIN32_WINNT=0x0601)
target_link_options(voiceime_tsf PRIVATE -static-libstdc++ -static-libgcc)
add_dependencies(voiceime_tests voiceime_tsf)

add_executable(voiceime_register src/register_ime.cpp)
target_link_libraries(voiceime_register PRIVATE ole32 uuid advapi32 "-Wl,-Bstatic" "-Wl,--whole-archive" "-lwinpthread" "-Wl,--no-whole-archive" "-Wl,-Bdynamic")
target_include_directories(voiceime_register PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(voiceime_register PRIVATE _WIN32_WINNT=0x0601)
target_link_options(voiceime_register PRIVATE -static-libstdc++ -static-libgcc)
