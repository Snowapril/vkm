# EmbedBinary.cmake
#
# Generates a C header embedding INPUT's raw bytes as a file-local byte array, so a
# build-time binary (e.g. a precompiled metallib) can be baked into the executable
# instead of shipped/located as a separate file at run time.
#
# Invoke via: cmake -DINPUT=<file> -DOUTPUT=<header> -DSYMBOL=<name> -P EmbedBinary.cmake
# Emits:
#   static const unsigned char <SYMBOL>[]  = { 0x.., ... };
#   static const unsigned long <SYMBOL>Len = <byte count>;

if (NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "EmbedBinary.cmake requires -DINPUT, -DOUTPUT and -DSYMBOL")
endif()

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hexLength)
math(EXPR _byteCount "${_hexLength} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")

file(WRITE "${OUTPUT}"
    "// Generated from ${INPUT} by EmbedBinary.cmake -- do not edit.\n"
    "static const unsigned char ${SYMBOL}[] = {\n${_bytes}\n};\n"
    "static const unsigned long ${SYMBOL}Len = ${_byteCount};\n")
