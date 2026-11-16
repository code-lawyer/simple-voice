# sherpa-onnx v2.0.1 Windows x64 shared MD. The import library is MSVC, so the
# model worker loads sherpa-onnx-c-api.dll at runtime instead of linking the .lib.
set(VOICEIME_SHERPA_ROOT
    "${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release")

function(voiceime_use_sherpa target)
  if(EXISTS "${VOICEIME_SHERPA_ROOT}/include/sherpa-onnx/c-api/c-api.h")
    target_include_directories(${target} PRIVATE "${VOICEIME_SHERPA_ROOT}/include")
    target_compile_definitions(${target} PRIVATE VOICEIME_HAS_SHERPA=1)
    foreach(dll sherpa-onnx-c-api.dll onnxruntime.dll onnxruntime_providers_shared.dll)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${VOICEIME_SHERPA_ROOT}/lib/${dll}"
          $<TARGET_FILE_DIR:${target}>)
    endforeach()
  else()
    target_compile_definitions(${target} PRIVATE VOICEIME_HAS_SHERPA=0)
    message(STATUS "sherpa-onnx headers missing; run tools/download_models.ps1 before a real engine")
  endif()
endfunction()
