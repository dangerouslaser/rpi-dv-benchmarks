# cmake -DIN=core.glsl -DOUT=core_glsl.h -DNAME=core_glsl -P embed.cmake: a file as a C string
file(READ "${IN}" s)
string(REPLACE "\\" "\\\\" s "${s}")
string(REPLACE "\"" "\\\"" s "${s}")
string(REPLACE "\n" "\\n\"\n\"" s "${s}")
file(WRITE "${OUT}" "static const char ${NAME}[] =\n\"${s}\";\n")
