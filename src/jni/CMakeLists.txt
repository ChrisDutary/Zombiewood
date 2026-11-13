# zw_jni: JNIEnv/JavaVM falsos en memoria guest + clases Java emuladas (T7).
add_library(zw_jni STATIC jni.cpp runtime.cpp env.cpp classes_android.cpp classes_game.cpp)
target_link_libraries(zw_jni PUBLIC zw_core zw_cpu zw_loader)
target_include_directories(zw_jni PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Diagnostico de T7: carga el .so con el JNI real, ejecuta JNI_OnLoad + la secuencia de arranque Java
# (nativeSetGPUInfo, GL2JNILib.init, Game.nativeInit, nativeInit opcionales) e imprime la cobertura.
add_executable(zw_jni_loadonly jni_loadonly_main.cpp)
target_link_libraries(zw_jni_loadonly PRIVATE zw_jni)

if(BUILD_TESTING)
  set(_zw_tjni ${CMAKE_SOURCE_DIR}/tests/jni)
  if(EXISTS ${_zw_tjni}/test_jni.cpp)
    add_executable(zw_test_jni ${_zw_tjni}/test_jni.cpp)
    target_include_directories(zw_test_jni PRIVATE ${_zw_tjni})
    target_link_libraries(zw_test_jni PRIVATE zw_jni)
    add_test(NAME jni COMMAND zw_test_jni)
  endif()
endif()
