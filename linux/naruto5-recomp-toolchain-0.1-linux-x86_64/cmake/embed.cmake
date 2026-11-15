file(READ "${SRC}" content)
file(WRITE "${OUT}" "extern const char ${NAME}[];\nconst char ${NAME}[] = R\"WGSL(${content})WGSL\";\n")
