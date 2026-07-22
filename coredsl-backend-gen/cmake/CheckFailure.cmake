execute_process(
  COMMAND "${TOOL}" --dump-ast "${INPUT}"
  RESULT_VARIABLE Result
  OUTPUT_VARIABLE Output
  ERROR_VARIABLE Actual)
if(Result EQUAL 0)
  message(FATAL_ERROR "tool unexpectedly succeeded:\n${Output}")
endif()
file(READ "${EXPECTED}" Expected)
string(REPLACE "${INPUT}" "<input>" Actual "${Actual}")
if(NOT Actual STREQUAL Expected)
  message(FATAL_ERROR "diagnostic differs\nexpected:\n${Expected}\nactual:\n${Actual}")
endif()
