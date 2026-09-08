foreach(Kind IN ITEMS h cpp)
  execute_process(
    COMMAND "${RENDERER}" "${TEMPLATES}/LegalizerInfo.${Kind}.mustache"
    OUTPUT_FILE "${OUTPUT_DIRECTORY}/ExampleLegalizerInfo.${Kind}"
    ERROR_VARIABLE Error
    RESULT_VARIABLE Result)
  if(NOT Result EQUAL 0)
    message(FATAL_ERROR "Legalizer example rendering failed: ${Error}")
  endif()
endforeach()
