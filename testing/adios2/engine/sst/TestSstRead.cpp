/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 */
#include <cstdint>
#include <cstring>

#include <iostream>
#include <stdexcept>

#include <adios2.h>

#include <gtest/gtest.h>

#include "TestData.h"

class SstReadTest : public ::testing::Test
{
public:
    SstReadTest() = default;
};

adios2::Params engineParams = {}; // parsed from command line

static std::string Trim(std::string &str)
{
    size_t first = str.find_first_not_of(' ');
    size_t last = str.find_last_not_of(' ');
    return str.substr(first, (last - first + 1));
}

/*
 * Engine parameters spec is a poor-man's JSON.  name:value pairs are separated
 * by commas.  White space is trimmed off front and back.  No quotes or anything
 * fancy allowed.
 */
static adios2::Params ParseEngineParams(std::string Input)
{
    std::istringstream ss(Input);
    std::string Param;
    adios2::Params Ret = {};

    while (std::getline(ss, Param, ','))
    {
        std::istringstream ss2(Param);
        std::string ParamName;
        std::string ParamValue;
        std::getline(ss2, ParamName, ':');
        if (!std::getline(ss2, ParamValue, ':'))
        {
            throw std::invalid_argument("Engine parameter \"" + Param +
                                        "\" missing value");
        }
        Ret[Trim(ParamName)] = Trim(ParamValue);
    }
    return Ret;
}

// ADIOS2 Sst read
TEST_F(SstReadTest, ADIOS2SstRead1D8)
{
    // Each process would write a 1x8 array and all processes would
    // form a mpiSize * Nx 1D array
    const std::string fname = "ADIOS2Sst";

    int mpiRank = 0, mpiSize = 1;

    // Number of steps
    const std::size_t NSteps = 1;

#ifdef ADIOS2_HAVE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &mpiRank);
    MPI_Comm_size(MPI_COMM_WORLD, &mpiSize);
#endif

// Write test data using ADIOS2

#ifdef ADIOS2_HAVE_MPI
    adios2::ADIOS adios(MPI_COMM_WORLD, adios2::DebugON);
#else
    adios2::ADIOS adios(true);
#endif
    adios2::IO io = adios.DeclareIO("TestIO");

    // Create the Engine
    io.SetEngine("Sst");
    io.SetParameters(engineParams);

    adios2::Engine engine = io.Open(fname, adios2::Mode::Read);

    unsigned int t = 0;

    while (engine.BeginStep() == adios2::StepStatus::OK)
    {
        const size_t currentStep = engine.CurrentStep();
        EXPECT_EQ(currentStep, static_cast<size_t>(t));

        int writerSize;

        auto var_i8 = io.InquireVariable<int8_t>("i8");
        EXPECT_TRUE(var_i8);
        ASSERT_EQ(var_i8.ShapeID(), adios2::ShapeID::GlobalArray);
        /* must be a multiple of Nx */
        ASSERT_EQ(var_i8.Shape()[0] % Nx, 0);

        /* take the first size as something that gives us writer size */
        writerSize = var_i8.Shape()[0] / 10;

        auto var_i16 = io.InquireVariable<int16_t>("i16");
        EXPECT_TRUE(var_i16);
        ASSERT_EQ(var_i16.ShapeID(), adios2::ShapeID::GlobalArray);
        ASSERT_EQ(var_i16.Shape()[0], writerSize * Nx);

        auto var_i32 = io.InquireVariable<int32_t>("i32");
        EXPECT_TRUE(var_i32);
        ASSERT_EQ(var_i32.ShapeID(), adios2::ShapeID::GlobalArray);
        ASSERT_EQ(var_i32.Shape()[0], writerSize * Nx);

        auto var_i64 = io.InquireVariable<int64_t>("i64");
        EXPECT_TRUE(var_i64);
        ASSERT_EQ(var_i64.ShapeID(), adios2::ShapeID::GlobalArray);
        ASSERT_EQ(var_i64.Shape()[0], writerSize * Nx);

        auto var_r32 = io.InquireVariable<float>("r32");
        EXPECT_TRUE(var_r32);
        ASSERT_EQ(var_r32.ShapeID(), adios2::ShapeID::GlobalArray);
        ASSERT_EQ(var_r32.Shape()[0], writerSize * Nx);

        auto var_r64 = io.InquireVariable<double>("r64");
        EXPECT_TRUE(var_r64);
        ASSERT_EQ(var_r64.ShapeID(), adios2::ShapeID::GlobalArray);
        ASSERT_EQ(var_r64.Shape()[0], writerSize * Nx);

        auto var_r64_2d = io.InquireVariable<double>("r64_2d");
        EXPECT_TRUE(var_r64_2d);
        ASSERT_EQ(var_r64_2d.ShapeID(), adios2::ShapeID::GlobalArray);
        ASSERT_EQ(var_r64_2d.Shape()[0], writerSize * Nx);
        ASSERT_EQ(var_r64_2d.Shape()[1], 2);

        long unsigned int myStart = (writerSize * Nx / mpiSize) * mpiRank;
        long unsigned int myLength =
            ((writerSize * Nx + mpiSize - 1) / mpiSize);

        if (myStart + myLength > writerSize * Nx)
        {
            myLength = writerSize * Nx - myStart;
        }
        const adios2::Dims start{myStart};
        const adios2::Dims count{myLength};
        const adios2::Dims start2{myStart, 0};
        const adios2::Dims count2{myLength, 2};

        const adios2::Box<adios2::Dims> sel(start, count);
        const adios2::Box<adios2::Dims> sel2(start2, count2);

        var_i8.SetSelection(sel);
        var_i16.SetSelection(sel);
        var_i32.SetSelection(sel);
        var_i64.SetSelection(sel);

        var_r32.SetSelection(sel);
        var_r64.SetSelection(sel);
        var_r64_2d.SetSelection(sel2);

        in_I8.reserve(myLength);
        in_I16.reserve(myLength);
        in_I32.reserve(myLength);
        in_I64.reserve(myLength);
        in_R32.reserve(myLength);
        in_R64.reserve(myLength);
        in_R64_2d.reserve(myLength * 2);
        engine.Get(var_i8, in_I8.data());
        engine.Get(var_i16, in_I16.data());
        engine.Get(var_i32, in_I32.data());
        engine.Get(var_i64, in_I64.data());

        engine.Get(var_r32, in_R32.data());
        engine.Get(var_r64, in_R64.data());
        engine.Get(var_r64_2d, in_R64_2d.data());

        engine.EndStep();

        EXPECT_EQ(validateSstTestData(myStart, myLength, t), 0);
        ++t;
    }

    EXPECT_EQ(t, NSteps);

    // Close the file
    engine.Close();
}

//******************************************************************************
// main
//******************************************************************************

int main(int argc, char **argv)
{
#ifdef ADIOS2_HAVE_MPI
    MPI_Init(nullptr, nullptr);
#endif

    int result;
    ::testing::InitGoogleTest(&argc, argv);

    if (argc > 1)
    {
        engineParams = ParseEngineParams(argv[1]);
    }

    result = RUN_ALL_TESTS();

#ifdef ADIOS2_HAVE_MPI
    MPI_Finalize();
#endif

    return result;
}
