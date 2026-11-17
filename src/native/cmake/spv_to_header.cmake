# Converts a SPIR-V binary file into a C++ header containing a uint32_t array.
if(NOT DEFINED INPUT_SPV OR NOT DEFINED OUTPUT_H OR NOT DEFINED VAR_NAME)
    message(FATAL_ERROR "INPUT_SPV, OUTPUT_H, and VAR_NAME must be defined")
endif()

file(READ "${INPUT_SPV}" HEX_CONTENTS HEX)
string(LENGTH "${HEX_CONTENTS}" HEX_LEN)
math(EXPR MOD_LEN "${HEX_LEN} % 8")
if(NOT MOD_LEN EQUAL 0)
    message(FATAL_ERROR "SPIR-V binary size must be a multiple of 4 bytes")
endif()

# Swap little-endian bytes to 32-bit hex literals using clean regex on 8-char chunks
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])"
       "0x\\4\\3\\2\\1u, " ARRAY_BODY "${HEX_CONTENTS}")

set(HEADER_TEXT "#pragma once\n#include <cstdint>\nalignas(4) inline constexpr uint32_t ${VAR_NAME}[] = {\n${ARRAY_BODY}\n};\n")
file(WRITE "${OUTPUT_H}" "${HEADER_TEXT}")
