# cmake -DIN=x.spv -DOUT=x.h -P embed.cmake: SPIR-V as a C array initialiser
# ({0x07230203, ...}), the form glslc -mfmt=c writes.
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" n)
math(EXPR rem "${n} % 8")
if(NOT rem EQUAL 0 OR n EQUAL 0)
  message(FATAL_ERROR "${IN}: not SPIR-V words")
endif()
# little-endian bytes -> 32-bit words
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])" "0x\\4\\3\\2\\1," words "${hex}")
string(REGEX REPLACE "((0x[0-9a-f]+,){8})" "\\1\n" words "${words}")
file(WRITE ${OUT} "{\n${words}\n}\n")
