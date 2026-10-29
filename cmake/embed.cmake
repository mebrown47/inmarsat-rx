# Writes OUT, a C file defining `const char NAME[]` with the bytes of IN (NUL-terminated).
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "// Generated from ${IN} by cmake/embed.cmake.\nconst char ${NAME}[] = {${bytes}0x00};\n")
