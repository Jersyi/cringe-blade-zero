# Build step of EnvCheck.exe: writes known_exes.h from the PBZ.exe builds proxy.cpp accepts (kExeBytes / kSha256 and the
# WeGame pair), so the checker and the DLL never disagree. update_port.py keeps rewriting them in proxy.cpp.
# cmake -DPROXY=<proxy.cpp> -DTEMPLATE=<known_exes.h.in> -DOUT=<known_exes.h> -P known_exes.cmake
file(READ "${PROXY}" source)
string(REGEX MATCH "kExeBytes = ([0-9]+);" match "${source}")
set(STEAM_BYTES "${CMAKE_MATCH_1}")
string(REGEX MATCH "kSha256 = \"([0-9a-f]+)\"" match "${source}")
set(STEAM_SHA "${CMAKE_MATCH_1}")
string(REGEX MATCH "kWeGameExeBytes = ([0-9]+);" match "${source}")
set(WEGAME_BYTES "${CMAKE_MATCH_1}")
string(REGEX MATCH "kWeGameSha256 = \"([0-9a-f]+)\"" match "${source}")
set(WEGAME_SHA "${CMAKE_MATCH_1}")
foreach(value STEAM_BYTES STEAM_SHA WEGAME_BYTES WEGAME_SHA)
    if("${${value}}" STREQUAL "")
        message(FATAL_ERROR "known_exes.cmake: ${value} not found in ${PROXY}")
    endif()
endforeach()
configure_file("${TEMPLATE}" "${OUT}" @ONLY)
