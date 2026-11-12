# zw_hle_audio: backend AudioTrack->SDL2 (audio_track) + imports OpenSL ES (T6).
add_library(zw_hle_audio STATIC audio_track.cpp opensl_stub.cpp)
target_link_libraries(zw_hle_audio PUBLIC zw_cpu SDL2::SDL2)
target_include_directories(zw_hle_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(BUILD_TESTING)
  set(_zw_tau ${CMAKE_SOURCE_DIR}/tests/hle_audio)
  if(EXISTS ${_zw_tau}/test_audio.cpp)
    add_executable(zw_test_audio ${_zw_tau}/test_audio.cpp)
    target_link_libraries(zw_test_audio PRIVATE zw_hle_audio)
    add_test(NAME hle_audio COMMAND zw_test_audio)
  endif()
endif()
