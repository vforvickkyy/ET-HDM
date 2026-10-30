# Eastape HFS engine: our sources + the vendored hfsfuse libraries.

option(EHFS_SHARED "Build the engine as a shared library (ehfs.dll)" OFF)

set(HFSFUSE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/hfsfuse/lib)

set(EHFS_SOURCES
  src/blockdev.c
  src/blockdev_posix.c
  src/blockdev_win32.c
  src/disk.c
  src/partition.c
  src/probe.c
  src/util.c
  src/volume.c
)

set(VENDOR_SOURCES
  src/vendor/libhfs_wrap.c
  src/vendor/unicode_wrap.c
  ${HFSFUSE}/libhfsuser/cache.c
  ${HFSFUSE}/libhfsuser/decmpfs.c
  ${HFSFUSE}/libhfsuser/features.c
  ${HFSFUSE}/libhfsuser/file.c
  ${HFSFUSE}/libhfsuser/hfsuser.c
  ${HFSFUSE}/utf8proc/utf8proc.c
  ${HFSFUSE}/LZFSE/src/lzfse_decode.c
  ${HFSFUSE}/LZFSE/src/lzfse_decode_base.c
  ${HFSFUSE}/LZFSE/src/lzfse_fse.c
  ${HFSFUSE}/LZFSE/src/lzvn_decode_base.c
  ${HFSFUSE}/LZVN/lzvn_decode.c
)

if(EHFS_SHARED)
  add_library(ehfs SHARED ${EHFS_SOURCES} ${VENDOR_SOURCES})
  target_compile_definitions(ehfs PUBLIC EHFS_SHARED PRIVATE EHFS_BUILDING)
  set_target_properties(ehfs PROPERTIES C_VISIBILITY_PRESET hidden)
else()
  add_library(ehfs STATIC ${EHFS_SOURCES} ${VENDOR_SOURCES})
endif()

target_include_directories(ehfs PUBLIC include PRIVATE src)

# libhfsuser has its own "features.h", which must not shadow the C library's
# <features.h>, so vendored directories are only searched for "quoted" includes.
foreach(dir libhfs libhfsuser utf8proc LZFSE/src LZVN)
  target_compile_options(ehfs PRIVATE "SHELL:-iquote ${HFSFUSE}/${dir}")
endforeach()

target_compile_definitions(ehfs PRIVATE
  _FILE_OFFSET_BITS=64
  HAVE_UTF8PROC
  UTF8PROC_STATIC
  HAVE_LZFSE=1
  HAVE_LZVN=1)

if(WIN32)
  # strtok_r and friends in MinGW-w64
  target_compile_definitions(ehfs PRIVATE _POSIX_THREAD_SAFE_FUNCTIONS)
else()
  target_compile_definitions(ehfs PRIVATE HAVE_PREAD=1)
endif()

# Vendored code keeps upstream's style; only our own sources get strict warnings.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set_source_files_properties(${EHFS_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")
  set_source_files_properties(${VENDOR_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

find_package(Threads REQUIRED)
target_link_libraries(ehfs PRIVATE Threads::Threads)

find_package(ZLIB)
if(ZLIB_FOUND)
  target_compile_definitions(ehfs PRIVATE HAVE_ZLIB=1)
  target_link_libraries(ehfs PRIVATE ZLIB::ZLIB)
else()
  message(WARNING "zlib not found: zlib-compressed HFS+ files will not be readable")
endif()
