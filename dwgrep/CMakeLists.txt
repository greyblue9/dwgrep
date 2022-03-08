
set(CMAKE_C_COMPILER )
set(CMAKE_CXX_COMPILER )

cmake_policy(SET CMP0053 NEW)
cmake_policy(SET CMP0056 NEW)
cmake_policy(SET CMP0066 NEW)
ADD_LIBRARY (AuxLib OBJECT
  ../libzwerg/strip.cc
  options.cc)

ADD_EXECUTABLE (dwgrep dwgrep.cc $<TARGET_OBJECTS:AuxLib>)
ADD_EXECUTABLE (dwgrep-genman genman.cc $<TARGET_OBJECTS:AuxLib>)
INCLUDE_DIRECTORIES (${CMAKE_SOURCE_DIR})
TARGET_LINK_LIBRARIES (dwgrep libzwerg)

INSTALL (TARGETS dwgrep RUNTIME DESTINATION bin)
