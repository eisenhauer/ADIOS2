#include <cstdint>
#include <cstring>
#include <ctime>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

#include <adios2.h>

#include <gtest/gtest.h>

#include "ParseArgs.h"

#include <adios2.h>
#include <numeric>
#include <vector>

class BigWriteTest : public ::testing::Test
{
public:
    BigWriteTest() = default;
};

#if ADIOS2_USE_MPI
MPI_Comm testComm;
#endif

// ADIOS2 COMMON write
TEST_F(BigWriteTest, ADIOS2BigWrite)
{
    // form a mpiSize * Nx 1D array
    int mpiRank = 0, mpiSize = 1;

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

    adios2::IO IO = adios.DeclareIO("IO");

    IO.SetParameter("QueueLimit", "1");
    IO.SetParameter("SpeculativePreloadMode", "off");
    IO.SetParameters(engineParams);
    IO.SetEngine(engine);

    adios2::Engine engine = IO.Open(fname, adios2::Mode::Write);

    using datatype = double;
    constexpr size_t vecLength = 2ull * 1024 * 1024 * 1024 / sizeof(double);

    std::vector<datatype> streamData(vecLength);

    std::iota(streamData.begin(), streamData.end(), 0.);

    auto variable = IO.DefineVariable<datatype>(
        "var", {vecLength}, {0}, {vecLength}, /* constantDims = */ true);


    for (unsigned step = 0; step < 2; ++step)
    {
        engine.BeginStep();
        engine.Put(variable, streamData.data());
        engine.EndStep();
    }
    engine.Close();
}

int main(int argc, char **argv)
{
#if ADIOS2_USE_MPI
    MPI_Init(nullptr, nullptr);

    int key;
    MPI_Comm_rank(MPI_COMM_WORLD, &key);

    const unsigned int color = 1;
    MPI_Comm_split(MPI_COMM_WORLD, color, key, &testComm);
#endif

    int result;
    ::testing::InitGoogleTest(&argc, argv);

    DelayMS = 0; // zero for common writer

    ParseArgs(argc, argv);

    result = RUN_ALL_TESTS();

#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif

    return result;
}
