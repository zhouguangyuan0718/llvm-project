execute_process(
  COMMAND "${TOOL}" --emit-llvm23-td "${INPUT}"
  RESULT_VARIABLE Result
  OUTPUT_VARIABLE Actual
  ERROR_VARIABLE Error)
if(NOT Result EQUAL 0)
  message(FATAL_ERROR "LLVM 23 TableGen emission failed:\n${Error}")
endif()
file(READ "${EXPECTED}" Expected)
if(NOT Actual STREQUAL Expected)
  message(FATAL_ERROR "LLVM 23 TableGen output differs\nexpected:\n${Expected}\nactual:\n${Actual}")
endif()
