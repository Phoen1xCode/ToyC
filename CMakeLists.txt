cmake_minimum_required(VERSION 3.13)
project(compiler LANGUAGES C CXX)

# settings
# set to OFF to enable C mode
set(CPP_MODE ON)
if(CPP_MODE)
  set(FB_EXT ".cpp")
else()
  set(FB_EXT ".c")
endif()
message(STATUS "Flex/Bison generated source file extension: ${FB_EXT}")

# enable all warnings
if(MSVC)
  add_compile_options(/W3)
else()
  # disable warnings caused by old version of Flex
  add_compile_options(-Wall -Wno-register)
endif()

# generate lexer/parser
file(GLOB_RECURSE L_SOURCES "src/*.l")
file(GLOB_RECURSE Y_SOURCES "src/*.y")
if(NOT (L_SOURCES STREQUAL "" AND Y_SOURCES STREQUAL ""))
  find_package(FLEX REQUIRED)
  find_package(BISON 3.0 REQUIRED)
  string(REGEX REPLACE ".*/(.*)\\.l" "${CMAKE_CURRENT_BINARY_DIR}/\\1.lex${FB_EXT}" L_OUTPUTS "${L_SOURCES}")
  string(REGEX REPLACE ".*/(.*)\\.y" "${CMAKE_CURRENT_BINARY_DIR}/\\1.tab${FB_EXT}" Y_OUTPUTS "${Y_SOURCES}")
  flex_target(Lexer ${L_SOURCES} ${L_OUTPUTS} DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/lexer.lex.hpp)
  bison_target(Parser ${Y_SOURCES} ${Y_OUTPUTS})
  add_flex_bison_dependency(Lexer Parser)
endif()

# project include directories
include_directories(src)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# all of C/C++ source files
file(GLOB_RECURSE C_SOURCES "src/*.c")
file(GLOB_RECURSE CXX_SOURCES "src/*.cpp")
file(GLOB_RECURSE CC_SOURCES "src/*.cc")
set(SOURCES ${C_SOURCES} ${CXX_SOURCES} ${CC_SOURCES}
            ${FLEX_Lexer_OUTPUTS} ${BISON_Parser_OUTPUT_SOURCE})

# executable
add_executable(compiler ${SOURCES})
set_target_properties(compiler PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
