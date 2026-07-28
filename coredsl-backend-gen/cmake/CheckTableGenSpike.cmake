execute_process(
  COMMAND "${TOOL}" --emit-td-spike "${INPUT}"
  RESULT_VARIABLE Result
  OUTPUT_VARIABLE Actual
  ERROR_VARIABLE Error)
if(NOT Result EQUAL 0)
  message(FATAL_ERROR "tool failed:\n${Error}")
endif()
file(READ "${EXPECTED}" Expected)
if(NOT Actual STREQUAL Expected)
  message(FATAL_ERROR
    "TableGen spike output differs\nexpected:\n${Expected}\nactual:\n${Actual}")
endif()
