# PRE_LINK, nicht Configure: auch wiederholte Builds ohne Reconfigure brauchen
# unterschiedliche Namen, solange ein Debugger die vorherige PDB geoeffnet hat.
foreach(_required IN ITEMS KROM_SCRIPT_PDB_DIR KROM_SCRIPT_PDB_TARGET KROM_SCRIPT_PDB_RESPONSE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${_required}")
    endif()
endforeach()
string(TIMESTAMP _stamp "%Y%m%d_%H%M%S" UTC)
string(RANDOM LENGTH 16 ALPHABET "0123456789abcdef" _nonce)
get_filename_component(_response_dir "${KROM_SCRIPT_PDB_RESPONSE}" DIRECTORY)
file(MAKE_DIRECTORY "${_response_dir}")
file(TO_CMAKE_PATH "${KROM_SCRIPT_PDB_DIR}" _pdb_dir)
file(WRITE "${KROM_SCRIPT_PDB_RESPONSE}"
    "/PDB:\"${_pdb_dir}/${KROM_SCRIPT_PDB_TARGET}_${_stamp}_${_nonce}.pdb\"\n")
