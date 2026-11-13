# libsmb2 as an ESP-IDF component, for the main app and sidecar/: the
# libsmb2/ submodule (github.com/sahlberg/libsmb2, pinned), as
# components/etl wraps etl/. Upstream's own CMakeLists.txt doesn't work as
# an IDF component ("add_definitions command is not scriptable"), so this
# one lists the sources. csim (csim/a800, csim/dstitch) builds the same
# submodule for the host with upstream's CMake.
#
# This directory used to be a separate, gitignored clone made by
# setup-build-env.sh. A checkout that still has that clone must move it
# aside before pulling: mv components/libsmb2 <somewhere else>.
set(SMB2_DIR "${CMAKE_CURRENT_LIST_DIR}/libsmb2")
if(NOT EXISTS "${SMB2_DIR}/lib/libsmb2.c")
    message(FATAL_ERROR "libsmb2: ${SMB2_DIR} is empty; run: git submodule update --init components/libsmb2/libsmb2")
endif()

idf_component_register(
  SRCS
    "${SMB2_DIR}/lib/aes.c"
    "${SMB2_DIR}/lib/aes128ccm.c"
    "${SMB2_DIR}/lib/alloc.c"
    "${SMB2_DIR}/lib/asn1-ber.c"
    "${SMB2_DIR}/lib/compat.c"
    "${SMB2_DIR}/lib/dcerpc.c"
    "${SMB2_DIR}/lib/dcerpc-lsa.c"
    "${SMB2_DIR}/lib/dcerpc-srvsvc.c"
    "${SMB2_DIR}/lib/errors.c"
    "${SMB2_DIR}/lib/init.c"
    "${SMB2_DIR}/lib/hmac.c"
    "${SMB2_DIR}/lib/hmac-md5.c"
    "${SMB2_DIR}/lib/libsmb2.c"
    "${SMB2_DIR}/lib/md4c.c"
    "${SMB2_DIR}/lib/md5.c"
    "${SMB2_DIR}/lib/ntlmssp.c"
    "${SMB2_DIR}/lib/pdu.c"
    "${SMB2_DIR}/lib/sha1.c"
    "${SMB2_DIR}/lib/sha224-256.c"
    "${SMB2_DIR}/lib/sha384-512.c"
    "${SMB2_DIR}/lib/smb2-cmd-close.c"
    "${SMB2_DIR}/lib/smb2-cmd-create.c"
    "${SMB2_DIR}/lib/smb2-cmd-echo.c"
    "${SMB2_DIR}/lib/smb2-cmd-error.c"
    "${SMB2_DIR}/lib/smb2-cmd-flush.c"
    "${SMB2_DIR}/lib/smb2-cmd-ioctl.c"
    "${SMB2_DIR}/lib/smb2-cmd-lock.c"
    "${SMB2_DIR}/lib/smb2-cmd-logoff.c"
    "${SMB2_DIR}/lib/smb2-cmd-negotiate.c"
    "${SMB2_DIR}/lib/smb2-cmd-notify-change.c"
    "${SMB2_DIR}/lib/smb2-cmd-query-directory.c"
    "${SMB2_DIR}/lib/smb2-cmd-query-info.c"
    "${SMB2_DIR}/lib/smb2-cmd-read.c"
    "${SMB2_DIR}/lib/smb2-cmd-session-setup.c"
    "${SMB2_DIR}/lib/smb2-cmd-set-info.c"
    "${SMB2_DIR}/lib/smb2-cmd-tree-connect.c"
    "${SMB2_DIR}/lib/smb2-cmd-tree-disconnect.c"
    "${SMB2_DIR}/lib/smb2-cmd-write.c"
    "${SMB2_DIR}/lib/smb2-data-file-info.c"
    "${SMB2_DIR}/lib/smb2-data-filesystem-info.c"
    "${SMB2_DIR}/lib/smb2-data-reparse-point.c"
    "${SMB2_DIR}/lib/smb2-data-security-descriptor.c"
    "${SMB2_DIR}/lib/smb2-share-enum.c"
    "${SMB2_DIR}/lib/smb3-seal.c"
    "${SMB2_DIR}/lib/smb2-signing.c"
    "${SMB2_DIR}/lib/socket.c"
    "${SMB2_DIR}/lib/spnego-wrapper.c"
    "${SMB2_DIR}/lib/sync.c"
    "${SMB2_DIR}/lib/timestamps.c"
    "${SMB2_DIR}/lib/unicode.c"
    "${SMB2_DIR}/lib/usha.c"
    "${SMB2_DIR}/lib/smb2-cmd-oplock-break.c"
  INCLUDE_DIRS
    "libsmb2/lib"
    "libsmb2/include"
    "libsmb2/include/smb2"
    "libsmb2/include/esp"
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    HAVE_CONFIG_H
    "_U_=__attribute__((unused))"
    NEED_READV NEED_WRITEV NEED_GETLOGIN_R NEED_RANDOM NEED_SRANDOM)
