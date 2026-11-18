cmake_minimum_required(VERSION 3.20)
project(SimpleVoice VERSION 0.2.0 LANGUAGES C CXX)
if(NOT WIN32 OR NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Simple Voice currently requires Windows x64 and a MinGW-w64 toolchain.")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libwinpthread.a
  OUTPUT_VARIABLE VOICEIME_PTHREAD_ARCHIVE OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
set(VOICEIME_PTHREAD_CORE "${CMAKE_BINARY_DIR}/runtime-core/libwinpthread.a")
add_custom_command(OUTPUT "${VOICEIME_PTHREAD_CORE}"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/runtime-core"
  COMMAND ${CMAKE_COMMAND} -E copy "${VOICEIME_PTHREAD_ARCHIVE}" "${VOICEIME_PTHREAD_CORE}"
  COMMAND ${CMAKE_AR} d "${VOICEIME_PTHREAD_CORE}" version.o
  DEPENDS "${VOICEIME_PTHREAD_ARCHIVE}" VERBATIM)
add_custom_target(voiceime_pthread_core DEPENDS "${VOICEIME_PTHREAD_CORE}")
if(MSVC)
  add_compile_options(/utf-8)
endif()

# cppjieba yanyiwu/cppjieba cbdf949eadb8887f060473b5dc6d3194655c8bef (MIT).
# Pinyin table: mozillazg/pinyin-data fe29ad32bdf352b31a02b9936e537f575432e9a6 (MIT),
# copied to third_party/cppjieba/dict/pinyin.txt. cppjieba has no pronunciation table.
add_library(voiceime_protocol STATIC
  src/utf8.cpp
  src/place.cpp
  src/ipc.cpp
  src/pipe_io.cpp
  src/ipc_security.cpp
  src/settings.cpp
  src/model_settings.cpp
  src/key_route.cpp)
target_include_directories(voiceime_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(voiceime_protocol PRIVATE _WIN32_WINNT=0x0601)
target_link_libraries(voiceime_protocol PUBLIC advapi32 psapi user32)

add_library(voiceime_core STATIC
  src/lexicon.cpp
  src/recognition_text.cpp
  src/segmenter.cpp
  src/worker_recognizer.cpp
  src/model_download.cpp
  src/data_backup.cpp
  src/resample.cpp
  src/session.cpp
  third_party/sqlite/sqlite3.c)
target_include_directories(voiceime_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cppjieba/include)
target_link_libraries(voiceime_core PUBLIC voiceime_protocol bcrypt)
target_compile_definitions(voiceime_core PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION _WIN32_WINNT=0x0601)
if(MSVC)
  set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS /w)
else()
  set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS -w)
endif()

add_executable(voiceime_tests tests/main.cpp tests/utf8_test.cpp tests/lexicon_test.cpp tests/segmenter_test.cpp tests/recording_test.cpp tests/session_test.cpp tests/place_test.cpp tests/ipc_test.cpp tests/tsf_load_test.cpp tests/settings_test.cpp tests/key_route_test.cpp tests/pinyin_controller_test.cpp tests/rime_session_test.cpp tests/candidate_test.cpp)
target_link_libraries(voiceime_tests PRIVATE voiceime_core)
target_sources(voiceime_tests PRIVATE tests/pipe_io_test.cpp tests/ipc_security_test.cpp tests/concurrency_test.cpp tests/hardening_test.cpp tests/text_safety_test.cpp)
target_sources(voiceime_tests PRIVATE tests/model_settings_test.cpp)
target_sources(voiceime_tests PRIVATE tests/data_backup_test.cpp)
target_sources(voiceime_tests PRIVATE tests/model_download_test.cpp)
enable_testing()
add_test(NAME voiceime.unit COMMAND voiceime_tests)
set_tests_properties(voiceime.unit PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 120)
target_include_directories(voiceime_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}" VOICEIME_SOURCE_DIR_CMAKE)
target_compile_definitions(voiceime_tests PRIVATE "VOICEIME_SOURCE_DIR=\"${VOICEIME_SOURCE_DIR_CMAKE}\"")

add_executable(voiceime_engine src/engine_main.cpp src/confirm_box.cpp src/hotkey_dialog.cpp src/preferences_window.cpp src/voiceime_engine.rc src/version.rc)
set_source_files_properties(src/voiceime_engine.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/voiceime_engine.manifest")
target_link_options(voiceime_engine PRIVATE "-specs=${CMAKE_SOURCE_DIR}/cmake/no-default-manifest.specs")
target_link_libraries(voiceime_engine PRIVATE voiceime_core ole32 uuid user32 gdi32 shell32 comctl32)
target_compile_definitions(voiceime_engine PRIVATE _WIN32_WINNT=0x0601)
include(cmake/sherpa.cmake)

add_executable(voiceime_model_worker src/model_worker_main.cpp src/sherpa_recognizer.cpp src/version.rc)
target_link_libraries(voiceime_model_worker PRIVATE voiceime_core)
target_compile_definitions(voiceime_model_worker PRIVATE _WIN32_WINNT=0x0601)
voiceime_use_sherpa(voiceime_model_worker)
add_dependencies(voiceime_engine voiceime_model_worker)
add_custom_command(TARGET voiceime_engine POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:voiceime_engine>/tools"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/tools/download_models.ps1" "${CMAKE_SOURCE_DIR}/tools/dependencies.json"
    "$<TARGET_FILE_DIR:voiceime_engine>/tools")

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
add_library(voiceime_tsf SHARED src/tsf_main.cpp src/tsf_service.cpp src/candidate_bar.cpp src/version.rc)
target_link_libraries(voiceime_tsf PRIVATE voiceime_protocol)
target_sources(voiceime_tsf PRIVATE src/input_indicator.cpp)
target_compile_definitions(voiceime_tsf PRIVATE SIMPLE_VOICE_DLL)
set_target_properties(voiceime_tsf PROPERTIES PREFIX "" OUTPUT_NAME "voiceime_tsf")
add_dependencies(voiceime_tsf voiceime_pthread_core)
target_link_libraries(voiceime_tsf PRIVATE ole32 oleaut32 uuid user32 gdi32 comctl32 "-Wl,-Bstatic" "-Wl,--whole-archive" "${VOICEIME_PTHREAD_CORE}" "-Wl,--no-whole-archive" "-Wl,-Bdynamic")
target_include_directories(voiceime_tsf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(voiceime_tsf PRIVATE _WIN32_WINNT=0x0601)
target_link_options(voiceime_tsf PRIVATE -static-libstdc++ -static-libgcc)
# EnsureRime reads rime-data beside the DLL. The source tree is not that directory.
add_custom_target(voiceime_rime_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_SOURCE_DIR}/rime-data"
    "${CMAKE_BINARY_DIR}/rime-data")
add_dependencies(voiceime_tsf voiceime_rime_data)
add_dependencies(voiceime_tests voiceime_tsf)

add_executable(voiceime_register src/register_ime.cpp src/register_ime.rc src/version.rc)
target_link_options(voiceime_register PRIVATE "-specs=${CMAKE_SOURCE_DIR}/cmake/no-default-manifest.specs")
add_dependencies(voiceime_register voiceime_pthread_core)
target_link_libraries(voiceime_register PRIVATE ole32 uuid advapi32 "-Wl,-Bstatic" "-Wl,--whole-archive" "${VOICEIME_PTHREAD_CORE}" "-Wl,--no-whole-archive" "-Wl,-Bdynamic")
target_include_directories(voiceime_register PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(voiceime_register PRIVATE _WIN32_WINNT=0x0601)
target_link_options(voiceime_register PRIVATE -static-libstdc++ -static-libgcc)

include(cmake/librime.cmake)
target_sources(voiceime_tests PRIVATE src/rime_session.cpp)
target_sources(voiceime_tsf PRIVATE src/rime_session.cpp)
voiceime_link_librime(voiceime_tests)
voiceime_link_librime(voiceime_tsf)
target_link_libraries(voiceime_tsf PRIVATE oleacc advapi32)

add_executable(voiceime_recognizer_smoke tools/recognizer_smoke.cpp src/sherpa_recognizer.cpp)
target_link_libraries(voiceime_recognizer_smoke PRIVATE voiceime_core psapi)
voiceime_use_sherpa(voiceime_recognizer_smoke)
voiceime_copy_runtime(voiceime_recognizer_smoke)

add_executable(voiceime_ui_check tools/ui_preview.cpp src/candidate_bar.cpp src/confirm_box.cpp src/hotkey_dialog.cpp src/preferences_window.cpp src/voiceime_engine.rc)
target_link_options(voiceime_ui_check PRIVATE "-specs=${CMAKE_SOURCE_DIR}/cmake/no-default-manifest.specs")
target_compile_definitions(voiceime_ui_check PRIVATE VOICEIME_UI_TESTING)
target_link_libraries(voiceime_ui_check PRIVATE voiceime_core gdiplus gdi32 user32 comctl32)
target_link_libraries(voiceime_ui_check PRIVATE ole32 oleaut32 oleacc uuid)
voiceime_copy_runtime(voiceime_ui_check)

# Test host uses the production engine with compile-time pipe/data isolation
# and synthetic capture. It never connects to the installed input method.
add_executable(voiceime_engine_testhost src/engine_main.cpp src/confirm_box.cpp src/hotkey_dialog.cpp src/preferences_window.cpp)
target_compile_definitions(voiceime_engine_testhost PRIVATE VOICEIME_TESTING _WIN32_WINNT=0x0601)
target_link_libraries(voiceime_engine_testhost PRIVATE voiceime_core ole32 uuid user32 gdi32 shell32 comctl32)
add_dependencies(voiceime_engine_testhost voiceime_model_worker)
voiceime_copy_runtime(voiceime_engine_testhost)
add_executable(voiceime_engine_integration tools/engine_integration.cpp)
target_link_libraries(voiceime_engine_integration PRIVATE voiceime_core)
voiceime_copy_runtime(voiceime_engine_integration)
add_dependencies(voiceime_engine_integration voiceime_engine_testhost)
add_test(NAME voiceime.engine COMMAND voiceime_engine_integration "${CMAKE_BINARY_DIR}/engine-integration")
set_tests_properties(voiceime.engine PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 90)

add_executable(voiceime_tsf_integration tools/tsf_integration.cpp src/rime_session.cpp src/candidate_bar.cpp)
target_sources(voiceime_tsf_integration PRIVATE src/input_indicator.cpp)
target_compile_definitions(voiceime_tsf_integration PRIVATE VOICEIME_TESTING _WIN32_WINNT=0x0601)
target_link_libraries(voiceime_tsf_integration PRIVATE voiceime_core ole32 oleaut32 uuid user32 gdi32 comctl32)
target_link_libraries(voiceime_tsf_integration PRIVATE oleacc advapi32)
voiceime_link_librime(voiceime_tsf_integration)
voiceime_copy_runtime(voiceime_tsf_integration)
add_dependencies(voiceime_tsf_integration voiceime_rime_data)
add_test(NAME voiceime.tsf COMMAND voiceime_tsf_integration "${CMAKE_BINARY_DIR}/tsf-integration")
set_tests_properties(voiceime.tsf PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 60)

# Run explicitly after installation: loads the registered DLL through COM and
# verifies its real engine. It never opens the microphone and is not a CTest.
add_executable(voiceime_installed_check tools/installed_check.cpp)
target_link_libraries(voiceime_installed_check PRIVATE voiceime_core ole32 oleaut32 uuid user32)
target_compile_definitions(voiceime_installed_check PRIVATE _WIN32_WINNT=0x0601)
voiceime_copy_runtime(voiceime_installed_check)

add_executable(voiceime_indicator_live_check tools/indicator_live_check.cpp)
target_include_directories(voiceime_indicator_live_check PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(voiceime_indicator_live_check PRIVATE ole32 uuid user32 gdi32 gdiplus)
voiceime_copy_runtime(voiceime_indicator_live_check)

add_executable(voiceime_popup_regression tools/popup_regression.cpp src/confirm_box.cpp src/candidate_bar.cpp)
target_compile_definitions(voiceime_popup_regression PRIVATE VOICEIME_UI_TESTING)
target_link_libraries(voiceime_popup_regression PRIVATE voiceime_core ole32 oleaut32 oleacc uuid user32 gdi32 comctl32)
voiceime_copy_runtime(voiceime_popup_regression)
add_test(NAME voiceime.popup COMMAND voiceime_popup_regression)
set_tests_properties(voiceime.popup PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}" TIMEOUT 30)

# Brand resources are embedded in binaries, including the production-code fixtures.
voiceime_copy_runtime(voiceime_model_worker)
add_executable(voiceime_model_worker_testhost src/model_worker_main.cpp src/sherpa_recognizer.cpp)
target_compile_definitions(voiceime_model_worker_testhost PRIVATE VOICEIME_WORKER_TESTING _WIN32_WINNT=0x0601)
target_link_libraries(voiceime_model_worker_testhost PRIVATE voiceime_core)
voiceime_use_sherpa(voiceime_model_worker_testhost)
voiceime_copy_runtime(voiceime_model_worker_testhost)
add_executable(voiceime_model_worker_integration tools/model_worker_integration.cpp)
target_include_directories(voiceime_model_worker_integration PRIVATE tests)
target_link_libraries(voiceime_model_worker_integration PRIVATE voiceime_core)
voiceime_copy_runtime(voiceime_model_worker_integration)
add_dependencies(voiceime_model_worker_integration voiceime_model_worker_testhost)
add_test(NAME voiceime.model_worker COMMAND voiceime_model_worker_integration)
set_tests_properties(voiceime.model_worker PROPERTIES TIMEOUT 30)

foreach(target voiceime_engine voiceime_tsf voiceime_register voiceime_engine_testhost voiceime_tsf_integration voiceime_ui_check)
  target_sources(${target} PRIVATE src/brand.rc)
endforeach()
foreach(target voiceime_engine voiceime_engine_testhost voiceime_ui_check)
  target_sources(${target} PRIVATE src/preferences_view.cpp)
  target_link_libraries(${target} PRIVATE uxtheme)
endforeach()
set_source_files_properties(src/brand.rc PROPERTIES OBJECT_DEPENDS
  "${CMAKE_SOURCE_DIR}/assets/brand/simple-voice.ico;${CMAKE_SOURCE_DIR}/assets/brand/sound-core-white.ico;${CMAKE_SOURCE_DIR}/assets/brand/sound-core-black.ico;${CMAKE_SOURCE_DIR}/assets/brand/mode-chinese.ico;${CMAKE_SOURCE_DIR}/assets/brand/mode-english.ico;${CMAKE_SOURCE_DIR}/assets/brand/mode-chinese-white.ico;${CMAKE_SOURCE_DIR}/assets/brand/mode-english-white.ico")
