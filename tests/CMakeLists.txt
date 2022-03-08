
set(CMAKE_C_COMPILER )
set(CMAKE_CXX_COMPILER )

cmake_policy(SET CMP0053 NEW)
cmake_policy(SET CMP0056 NEW)
cmake_policy(SET CMP0066 NEW)
ADD_TEST (RegressionTests
  ${CMAKE_SOURCE_DIR}/tests/tests.sh
  ${CMAKE_CURRENT_BINARY_DIR}/../dwgrep/dwgrep)
