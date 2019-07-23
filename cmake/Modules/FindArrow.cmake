#.rst:
# FindArrow.cmake
# -------------
#
# Find a Arrow installation.
#
# This module finds if Arrow is installed and selects a default
# configuration to use.
#
# find_package(Arrow ...)
#
#
# The following variables control which libraries are found::
#
#   Arrow_USE_STATIC_LIBS  - Set to ON to force use of static libraries.
#
# The following are set after the configuration is done:
#
# ::
#
#   Arrow_FOUND            - Set to TRUE if Arrow was found.
#   Arrow_LIBRARIES        - Path to the Arrow libraries.
#   Arrow_LIBRARY_DIRS     - compile time link directories
#   Arrow_INCLUDE_DIRS     - compile time include directories
#
#
# Sample usage:
#
# ::
#
#    find_package(Arrow)
#    if(Arrow_FOUND)
#      target_link_libraries(<YourTarget> ${Arrow_LIBRARIES})
#    endif()

if(Arrow_USE_STATIC_LIBS)
  set(_CMAKE_FIND_LIBRARY_SUFFIXES ${CMAKE_FIND_LIBRARY_SUFFIXES})
  set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})
endif()


find_library(Arrow_LIBRARY
  NAMES arrow
  HINTS
  ENV LD_LIBRARY_PATH
  ENV DYLD_LIBRARY_PATH
  PATHS
  /usr/lib
  /usr/local/lib
  /usr/local/homebrew/lib
  /opt/local/lib)

find_library(Arrow_DC_LIBRARY
  NAMES double-conversion
  HINTS
  ENV LD_LIBRARY_PATH
  ENV DYLD_LIBRARY_PATH
  PATHS
  /usr/lib
  /usr/local/lib
  /usr/local/homebrew/lib
  /opt/local/lib)

find_library(Arrow_GPU_LIBRARY
  NAMES arrow_gpu
  HINTS
  ENV LD_LIBRARY_PATH
  ENV DYLD_LIBRARY_PATH
  PATHS
  /usr/lib
  /usr/local/lib
  /usr/local/homebrew/lib
  /opt/local/lib)

get_filename_component(Arrow_LIBRARY_DIR ${Arrow_LIBRARY} DIRECTORY)

# Set standard CMake FindPackage variables if found.
set(Arrow_LIBRARIES ${Arrow_LIBRARY} ${Arrow_DC_LIBRARY})
set(Arrow_GPU_LIBRARIES ${Arrow_GPU_LIBRARY})
set(Arrow_LIBRARY_DIRS ${Arrow_LIBRARY_DIR})
set(Arrow_INCLUDE_DIRS ${Arrow_LIBRARY_DIR}/../include)

find_package(Snappy)
if(Snappy_FOUND)
  list(APPEND Arrow_LIBRARIES ${Snappy_LIBRARIES})
endif()

if(Arrow_USE_STATIC_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ${_CMAKE_FIND_LIBRARY_SUFFIXES})
endif()

try_compile(HAVE_ARROW_STATIC_RECORDBATCH_CTOR
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/cmake/Modules/arrow_static_recordbatch.cpp
  COMPILE_DEFINITIONS -I${Arrow_INCLUDE_DIRS} -std=c++14
  LINK_LIBRARIES ${Arrow_LIBRARY})

try_compile(HAVE_ARROW_APPENDVALUES
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/cmake/Modules/arrow_appendvalues.cpp
  COMPILE_DEFINITIONS -I${Arrow_INCLUDE_DIRS} -std=c++14
  LINK_LIBRARIES ${Arrow_LIBRARY})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Arrow REQUIRED_VARS Arrow_LIBRARY)
