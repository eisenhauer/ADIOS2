#------------------------------------------------------------------------------#
# Distributed under the OSI-approved Apache License, Version 2.0.  See
# accompanying file Copyright.txt for details.
#------------------------------------------------------------------------------#

cmake_minimum_required(VERSION 3.12)
project(ADIOS2HelloBPStepsWriteReadDerived)

if(NOT TARGET adios2_core)
  set(_components CXX)

  find_package(MPI COMPONENTS C)
  if(MPI_FOUND)
    # Workaround for various MPI implementations forcing the link of C++ bindings
    add_definitions(-DOMPI_SKIP_MPICXX -DMPICH_SKIP_MPICXX)

    list(APPEND _components MPI)
  endif()

  find_package(ADIOS2 REQUIRED COMPONENTS ${_components})
endif()

add_executable(adios2_hello_bpStepsWriteReadDerived bpStepsWriteReadDerived.cpp)
target_link_libraries(adios2_hello_bpStepsWriteReadDerived adios2::cxx11)
install(TARGETS adios2_hello_bpStepsWriteReadDerived RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(ADIOS2_HAVE_MPI)
    add_executable(adios2_hello_bpStepsWriteReadDerived_mpi bpStepsWriteReadDerived.cpp)
    target_link_libraries(adios2_hello_bpStepsWriteReadDerived_mpi adios2::cxx11_mpi MPI::MPI_CXX)
    install(TARGETS adios2_hello_bpStepsWriteReadDerived_mpi RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
