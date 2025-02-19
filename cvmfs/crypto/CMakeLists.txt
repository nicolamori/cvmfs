if (BUILD_CVMFS OR BUILD_SERVER OR BUILD_RECEIVER OR
    BUILD_SHRINKWRAP OR BUILD_PRELOADER OR BUILD_UNITTESTS)

set (LIBCVMFS_CRYPTO_SOURCES
     crypto/crypto_util.cc
     crypto/hash.cc
     crypto/encrypt.cc
     crypto/signature.cc
)

set (LIBCVMFS_CRYPTO_CFLAGS "-D_FILE_OFFSET_BITS=64 -DCVMFS_LIBRARY -DCVMFS_RAISE_EXCEPTIONS -fexceptions")
set (LIBCVMFS_CRYPTO_LINK_LIBRARIES "")
list (APPEND LIBCVMFS_CRYPTO_LINK_LIBRARIES
      ${Libcrypto_LIBRARIES}
      ${SHA3_LIBRARIES}
      ${RT_LIBRARY}
      pthread
      dl)

add_library(cvmfs_crypto SHARED ${LIBCVMFS_CRYPTO_SOURCES})
set_target_properties (cvmfs_crypto PROPERTIES
    VERSION ${CernVM-FS_VERSION_STRING}
    COMPILE_FLAGS "${LIBCVMFS_CRYPTO_CFLAGS}")
target_include_directories (cvmfs_crypto PRIVATE ${Libcrypto_INCLUDE_DIRS})
target_link_libraries (cvmfs_crypto
                       cvmfs_util
                       ${LIBCVMFS_CRYPTO_LINK_LIBRARIES}
)

install (TARGETS cvmfs_crypto LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})


if (BUILD_CVMFS OR BUILD_SERVER_DEBUG OR BUILD_RECEIVER_DEBUG OR BUILD_UNITTESTS_DEBUG)
  add_library (cvmfs_crypto_debug SHARED ${LIBCVMFS_CRYPTO_SOURCES})
  set_target_properties (cvmfs_crypto_debug PROPERTIES
      VERSION ${CernVM-FS_VERSION_STRING}
      COMPILE_FLAGS "${LIBCVMFS_CRYPTO_CFLAGS} -O0 -DDEBUGMSG")
  target_include_directories (cvmfs_crypto_debug PRIVATE ${Libcrypto_INCLUDE_DIRS})
  target_link_libraries (cvmfs_crypto_debug
                         cvmfs_util_debug
                         ${LIBCVMFS_CRYPTO_LINK_LIBRARIES}
  )
  install (TARGETS cvmfs_crypto_debug LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif () # debug binaries


endif () # components that need libcvmfs-crypto
