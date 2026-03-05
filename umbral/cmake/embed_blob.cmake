# Embeds a binary file as a C++ unsigned char array.
# Usage: cmake -DINPUT=<path> -DOUTPUT=<path> -DSYMBOL=<name> -P embed_blob.cmake

file(READ "${INPUT}" content HEX)
string(LENGTH "${content}" hex_len)
math(EXPR byte_count "${hex_len} / 2")
string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," content "${content}")

file(WRITE "${OUTPUT}"
"#include <cstddef>
extern \"C\" {
const unsigned char ${SYMBOL}[] = {
${content}
};
const std::size_t ${SYMBOL}_size = ${byte_count}u;
}
")
