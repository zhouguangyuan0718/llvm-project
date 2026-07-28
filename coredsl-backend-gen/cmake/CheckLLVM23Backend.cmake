if(NOT DEFINED TOOL OR NOT DEFINED INPUT OR NOT DEFINED OUTPUT_DIRECTORY)
  message(FATAL_ERROR "TOOL, INPUT, and OUTPUT_DIRECTORY are required")
endif()

# The emitter intentionally refuses to replace a directory.  This is an
# isolated CTest-owned path, so it is safe to recreate it for repeated runs.
file(REMOVE_RECURSE "${OUTPUT_DIRECTORY}")

execute_process(
  COMMAND "${TOOL}" "--emit-llvm23-backend=${OUTPUT_DIRECTORY}" "${INPUT}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "backend emission failed (${result}):\n${output}${error}")
endif()

foreach(file
    CMakeLists.txt
    Tiny32.td
    Tiny32Target.cpp
    TargetInfo/CMakeLists.txt
    TargetInfo/Tiny32TargetInfo.h
    TargetInfo/Tiny32TargetInfo.cpp)
  if(NOT EXISTS "${OUTPUT_DIRECTORY}/${file}")
    message(FATAL_ERROR "generated backend is missing ${file}")
  endif()
endforeach()

file(READ "${OUTPUT_DIRECTORY}/Tiny32Target.cpp" target_source)
foreach(required
    "LLVMInitializeTiny32Target()"
    "LLVMInitializeTiny32TargetMC()"
    "Tiny32CallLowering"
    "Tiny32RegisterBankInfo"
    "Tiny32PassConfig"
    "initializeGlobalISel"
    "TargetOpcode::G_ADD"
    "Tiny32::ADD"
    "TargetOpcode::G_SUB"
    "Tiny32::SUB")
  string(FIND "${target_source}" "${required}" required_offset)
  if(required_offset EQUAL -1)
    message(FATAL_ERROR "generated target source lacks '${required}'")
  endif()
endforeach()

file(READ "${OUTPUT_DIRECTORY}/Tiny32.td" target_description)
string(FIND "${target_description}" "def COREDSL_RET" return_offset)
if(return_offset EQUAL -1)
  message(FATAL_ERROR "generated target description lacks COREDSL_RET")
endif()
