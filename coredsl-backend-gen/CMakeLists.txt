cmake_minimum_required(VERSION 3.20)

project(CoreDSLBackendGen LANGUAGES CXX)

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
target_compile_features(coredsl_frontend PUBLIC cxx_std_17)

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
    NAME coredsl-parser-control-flow
    COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:coredsl-backend-gen>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/control-flow.core_desc
      -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/test/Parser/control-flow.ast
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckAST.cmake)
endif()
