#include <adios2.h>
#include <string>
#include <vector>

#if ADIOS2_USE_MPI
#include <mpi.h>
#endif

int main(int argc, char *argv[])
{
#if ADIOS2_USE_MPI
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    adios2::ADIOS adios(MPI_COMM_WORLD);
#else
    adios2::ADIOS adios;
    int rank = 0;
#endif
    adios2::IO IO = adios.DeclareIO("parallel_write_zero_extent");

    adios2::Engine writer = IO.Open("parallel_write_zero_extent.bp", adios2::Mode::Write);
    IO.DefineAttribute<unsigned char>("__openPMD_internal/useModifiableAttributes", 1);

    std::vector<double> pos_x = {40.0};
    std::vector<unsigned long long> posOff_x = {40};

    adios2::Variable<double> position_x;
    adios2::Variable<unsigned long long> positionOffset_x;

    for (size_t i = 0; i < 10; ++i)
    {
        writer.BeginStep();
        IO.DefineAttribute("__openPMD_groups/", (unsigned long long)i, "", "/", true);
        if (rank > 0)
        {
            position_x =
                i == 0 ? IO.DefineVariable<double>("/data/particles/e/position/x", {1}, {0}, {1})
                       : IO.InquireVariable<double>("/data/particles/e/position/x");
            IO.DefineAttribute("/data/particles/e/position/x/unitSI", (double)1, "", "/", true);
            IO.DefineAttribute("/data/particles/e/position/timeOffset", (float)0, "", "/", true);
            std::vector<double> unitDimension1{1, 0, 0, 0, 0, 0, 0};
            IO.DefineAttribute("/data/particles/e/position/unitDimension", unitDimension1.data(),
                               unitDimension1.size(), "", "/", true);
            IO.DefineAttribute("__openPMD_groups/data/particles/e/positionOffset",
                               (unsigned long long)i, "", "/", true);
            positionOffset_x =
                i == 0
                    ? IO.DefineVariable<unsigned long long>("/data/particles/e/positionOffset/x",
                                                            {1}, {0}, {1})
                    : IO.InquireVariable<unsigned long long>("/data/particles/e/positionOffset/x");
            IO.DefineAttribute("/data/particles/e/positionOffset/x/unitSI", (double)1, "", "/",
                               true);
            IO.DefineAttribute("/data/particles/e/positionOffset/timeOffset", (float)0, "", "/",
                               true);
            std::vector<double> unitDimension2{1, 0, 0, 0, 0, 0, 0};
            IO.DefineAttribute("/data/particles/e/positionOffset/unitDimension",
                               unitDimension2.data(), unitDimension2.size(), "", "/", true);
        }
        IO.DefineAttribute("/data/dt", (double)1, "", "/", true);
        IO.DefineAttribute("/data/time", (double)0, "", "/", true);
        IO.DefineAttribute("/data/timeUnitSI", (double)1, "", "/", true);
        if (rank > 0)
        {
            writer.Put(position_x, pos_x.data());
            writer.Put(positionOffset_x, posOff_x.data());
        }
        writer.EndStep();
    }

    writer.Close();
#if ADIOS2_USE_MPI
    MPI_Finalize();
#endif
    return 0;
}
