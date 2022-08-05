/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 */
#include <cstdint>
#include <cstring>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <adios2.h>

#include <gtest/gtest.h>

#include "TestData.h"

#include "ParseArgs.h"

class CommonReadTest : public ::testing::Test
{
public:
    CommonReadTest() = default;
};

typedef std::chrono::duration<double> Seconds;

#if ADIOS2_USE_MPI
MPI_Comm testComm;
#endif

// ADIOS2 Common read
TEST_F(CommonReadTest, ADIOS2CommonRead1D8)
{
    // Each process would write a 1x8 array and all processes would
    // form a mpiSize * Nx 1D array
    int mpiRank = 0, mpiSize = 1;
    int count = 0;

#if ADIOS2_USE_MPI
    MPI_Comm_rank(testComm, &mpiRank);
    MPI_Comm_size(testComm, &mpiSize);
#endif

    // Write test data using ADIOS2

#if ADIOS2_USE_MPI
    adios2::ADIOS adios(testComm);
#else
    adios2::ADIOS adios;
#endif
    using datatype = double;
    adios2::IO IO = adios.DeclareIO("IO");

    IO.SetParameter("SpeculativePreloadMode", "off");

    IO.SetEngine(engine);

    adios2::Engine engine = IO.Open(fname, adios2::Mode::Read);

    std::vector<datatype> streamData;

    unsigned currentStep = 0;

    while (engine.BeginStep() == adios2::StepStatus::OK)
    {
	auto variable = IO.InquireVariable<datatype>("var");
	if (!variable)
	{
		throw std::runtime_error("[Reader] Failed inquiring variable");
	}
	streamData.resize(variable.Shape()[0]);
        engine.Get(variable, streamData.data());
        engine.EndStep();
        std::cout << currentStep++ << std::endl;
	count++;
    }
    EXPECT_EQ(count, 2);
    engine.Close();
}

//******************************************************************************
// main
//******************************************************************************

int main(int argc, char **argv)
{
#if ADIOS2_USE_MPI
    MPI_Init(nullptr, nullptr);

    int key;
    MPI_Comm_rank(MPI_COMM_WORLD, &key);

    const unsigned int color = 2;
    MPI_Comm_split(MPI_COMM_WORLD, color, key, &testComm);
#endif

    int result;
    ::testing::InitGoogleTest(&argc, argv);
    ParseArgs(argc, argv);

    result = RUN_ALL_TESTS();

#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif

    return result;
}
