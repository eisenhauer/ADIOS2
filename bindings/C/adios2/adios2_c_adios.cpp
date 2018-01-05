/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * adios2_c_adios.cpp
 *
 *  Created on: Nov 8, 2017
 *      Author: William F Godoy godoywf@ornl.gov
 */

#include "adios2_c_adios.h"
#include "adios2/ADIOSMPI.h"
#include "adios2/core/ADIOS.h"

adios2_adios *adios2_init_config(const char *config_file, MPI_Comm mpi_comm,
                                 const adios2_debug_mode debug_mode)
{
    const bool debugBool = (debug_mode == adios2_debug_mode_on) ? true : false;
    adios2_adios *adios = reinterpret_cast<adios2_adios *>(
        new adios2::ADIOS(config_file, mpi_comm, debugBool, "C"));

    return adios;
}

adios2_adios *adios2_init(MPI_Comm mpi_comm, const adios2_debug_mode debug_mode)
{
    return adios2_init_config("", mpi_comm, debug_mode);
}

adios2_adios *adios2_init_config_nompi(const char *config_file,
                                       const adios2_debug_mode debug_mode)
{
    return adios2_init_config(config_file, MPI_COMM_SELF, debug_mode);
}

adios2_adios *adios2_init_nompi(const adios2_debug_mode debug_mode)
{
    return adios2_init_config("", MPI_COMM_SELF, debug_mode);
}

adios2_io *adios2_declare_io(adios2_adios *adios, const char *ioName)
{
    adios2_io *io = reinterpret_cast<adios2_io *>(
        &reinterpret_cast<adios2::ADIOS *>(adios)->DeclareIO(ioName));
    return io;
}

void adios2_finalize(adios2_adios *adios)
{
    delete reinterpret_cast<adios2::ADIOS *>(adios);
    adios = nullptr;
}
