# cmake/VerifyCookedArtifact.cmake
#
# Verifies a cooked artifact's `RuntimeHeader` (per ADR-0017) matches the
# expected magic, kind, and UUID. Used by the P3 task 4 smoke test to
# assert tide_cooker emits a structurally valid file end-to-end.
#
# Required cache vars:
#   ARTIFACT          — path to the .tide file produced by tide_cooker
#   EXPECTED_MAGIC    — 4 hex bytes, e.g. "54494445" for 'TIDE' little-endian
#   EXPECTED_KIND     — uint32, e.g. "02000000" for AssetKind::Mesh LE
#   EXPECTED_UUID_HEX — 32 hex chars (no dashes), the UUID's raw bytes

if(NOT EXISTS "${ARTIFACT}")
    message(FATAL_ERROR "cooked artifact not found: ${ARTIFACT}")
endif()

file(SIZE "${ARTIFACT}" _size)
if(_size LESS 48)
    message(FATAL_ERROR
        "cooked artifact too small (got ${_size} bytes, need >= 48 for RuntimeHeader)")
endif()

# Read the first 48 bytes (the header) as one hex string, lowercase.
file(READ "${ARTIFACT}" _head HEX LIMIT 48)

# Magic: bytes 0..3
string(SUBSTRING "${_head}" 0 8 _magic)
if(NOT _magic STREQUAL EXPECTED_MAGIC)
    message(FATAL_ERROR
        "magic mismatch: got '${_magic}', expected '${EXPECTED_MAGIC}'")
endif()

# Kind: bytes 4..7
string(SUBSTRING "${_head}" 8 8 _kind)
if(NOT _kind STREQUAL EXPECTED_KIND)
    message(FATAL_ERROR
        "kind mismatch: got '${_kind}', expected '${EXPECTED_KIND}'")
endif()

# UUID: bytes 32..47 (16 bytes = 32 hex chars)
string(SUBSTRING "${_head}" 64 32 _uuid)
if(NOT _uuid STREQUAL EXPECTED_UUID_HEX)
    message(FATAL_ERROR
        "uuid mismatch: got '${_uuid}', expected '${EXPECTED_UUID_HEX}'")
endif()

message(STATUS "tide_cooker_smoke: artifact OK (magic=${_magic} kind=${_kind} uuid=${_uuid})")
