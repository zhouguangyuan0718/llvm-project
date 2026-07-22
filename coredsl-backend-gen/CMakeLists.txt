cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(CoreDSLBackendGen LANGUAGES C CXX)

  find_package(LLVM REQUIRED CONFIG)
  if(NOT LLVM_PACKAGE_VERSION MATCHES "^23\\.")
    message(FATAL_ERROR "coredsl-backend-gen requires LLVM 23, found ${LLVM_PACKAGE_VERSION}")
  endif()
  message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} Support library")
else()
  if(NOT DEFINED LLVM_MAIN_SRC_DIR)
    message(FATAL_ERROR "coredsl-backend-gen must be configured from an LLVM build")
  endif()
  message(STATUS "Using in-tree LLVM Support library")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(coredsl_frontend
  lib/AST/ASTPrinter.cpp
  lib/Frontend/Diagnostics.cpp
  lib/Frontend/Lexer.cpp
  lib/Frontend/Parser.cpp
)
target_include_directories(coredsl_frontend PUBLIC include)
target_include_directories(coredsl_frontend SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_features(coredsl_frontend PUBLIC cxx_std_17)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  separate_arguments(COREDSL_LLVM_DEFINITIONS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  target_compile_options(coredsl_frontend PUBLIC ${COREDSL_LLVM_DEFINITIONS})
endif()
target_link_libraries(coredsl_frontend PUBLIC LLVMSupport)

add_executable(coredsl-backend-gen tools/coredsl-backend-gen/Main.cpp)
target_link_libraries(coredsl-backend-gen PRIVATE coredsl_frontend)

include(CTest)
if(BUILD_TESTING)
  add_test(
    NAME coredsl-parser-basic
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:coredsl-backend-gen>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/basic.core_desc
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/basic.ast
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckAST.cmake)
  add_test(
    NAME coredsl-parser-invalid
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:coredsl-backend-gen>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/invalid.core_desc
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/invalid.err
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckFailure.cmake)
  add_test(
    NAME coredsl-parser-unsupported-member
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:coredsl-backend-gen>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/unsupported-member.core_desc
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/unsupported-member.err
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckFailure.cmake)
endif()
