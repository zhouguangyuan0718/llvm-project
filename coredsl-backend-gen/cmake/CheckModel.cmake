execute_process(
  COMMAND "${TOOL}" --dump-model "${INPUT}"
  RESULT_VARIABLE Result
  OUTPUT_VARIABLE Actual
  ERROR_VARIABLE Error)
if(NOT Result EQUAL 0)
  message(FATAL_ERROR "model lowering failed:\n${Error}")
endif()
file(READ "${EXPECTED}" Expected)
if(NOT Actual STREQUAL Expected)
  message(FATAL_ERROR "model output differs\nexpected:\n${Expected}\nactual:\n${Actual}")
endif()
