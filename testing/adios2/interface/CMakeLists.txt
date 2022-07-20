#------------------------------------------------------------------------------#
# Distributed under the OSI-approved Apache License, Version 2.0.  See
# accompanying file Copyright.txt for details.
#------------------------------------------------------------------------------#

# These tests should be *very* fast
set(CTEST_TEST_TIMEOUT 10)

set(BP3_DIR ${CMAKE_CURRENT_BINARY_DIR}/bp3)
set(BP4_DIR ${CMAKE_CURRENT_BINARY_DIR}/bp4)
set(BP5_DIR ${CMAKE_CURRENT_BINARY_DIR}/bp5)

file(MAKE_DIRECTORY ${BP3_DIR})
file(MAKE_DIRECTORY ${BP4_DIR})
file(MAKE_DIRECTORY ${BP5_DIR})

macro(interface_gtest_add_tests_helper testname mpi)
  gtest_add_tests_helper(${testname} ${mpi} ADIOS Interface. .BP3
    WORKING_DIRECTORY ${BP3_DIR} EXTRA_ARGS "BP3"
  )
#  gtest_add_tests_helper(${testname} ${mpi} ADIOS Interface. .BP4
#    WORKING_DIRECTORY ${BP4_DIR} EXTRA_ARGS "BP4"
#  )
  if(ADIOS2_HAVE_BP5)
    gtest_add_tests_helper(${testname} ${mpi} ADIOS Interface. .BP5
      WORKING_DIRECTORY ${BP5_DIR} EXTRA_ARGS "BP5"
    )
  endif()
endmacro()

interface_gtest_add_tests_helper(Interface MPI_ALLOW)
#interface_gtest_add_tests_helper(Write MPI_ALLOW)
#interface_gtest_add_tests_helper(DefineVariable MPI_ALLOW)
#interface_gtest_add_tests_helper(DefineAttribute MPI_ALLOW)
#interface_gtest_add_tests_helper(Selection MPI_NONE)
#interface_gtest_add_tests_helper(NoMpi MPI_NONE)
