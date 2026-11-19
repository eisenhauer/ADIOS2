/*
 * SPDX-FileCopyrightText: 2026 Oak Ridge National Laboratory and Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "adios2/helper/adiosComm.h"
#include <memory>

#include "SstParamParser.h"
#include "SstWriter.h"
#include "SstWriter.tcc"
#include "adios2/toolkit/format/buffer/malloc/MallocV.h"
#include <adios2-perfstubs-interface.h>

namespace adios2
{
namespace core
{
namespace engine
{

SstWriter::SstWriter(IO &io, const std::string &name, const Mode mode, helper::Comm comm)
: Engine("SstWriter", io, name, mode, std::move(comm)), m_Profiler(m_Comm)
{
    Init();

    // GathervArraysTwoLevel needs at least two groups, so 4 ranks
    if ((m_Comm.Size() > Params.OneLevelGatherRanksLimit) && (m_Comm.Size() >= 4))
    {
        format::BP5Helper::InitTwoLevelComms(m_Comm, m_CommMetadataGroup,
                                             m_CommMetadataAggregators);
        m_Output = SstWriterOpenTwoLevel(name.c_str(), &Params, &m_Comm, &m_CommMetadataGroup,
                                         &m_CommMetadataAggregators, &m_Profiler);
    }
    else
    {
        m_Output = SstWriterOpen(name.c_str(), &Params, &m_Comm);
    }
    if (!m_Output)
    {
        helper::Throw<std::runtime_error>("Engine", "SstWriter", "SstWriter",
                                          "SstWriter did not open stream " + name);
    }
    m_IsOpen = true;
}

SstWriter::~SstWriter()
{
    if (m_IsOpen)
    {
        DestructorClose(m_FailVerbose);
    }
    SstStreamDestroy(m_Output);
}

StepStatus SstWriter::BeginStep(StepMode mode, const float timeout_sec)
{
    PERFSTUBS_SCOPED_TIMER_FUNC();
    m_WriterStep++;
    if (m_BetweenStepPairs)
    {
        helper::Throw<std::logic_error>("Engine", "SstWriter", "BeginStep",
                                        "BeginStep() is called a second time "
                                        "without an intervening EndStep()");
    }

    m_BetweenStepPairs = true;
    m_BP5Serializer.InitStep(new format::MallocV("SstWriter", true));
    return StepStatus::OK;
}

size_t SstWriter::CurrentStep() const { return m_WriterStep; }

void SstWriter::NotifyEngineAttribute(std::string name, DataType type) noexcept
{
    helper::Throw<std::invalid_argument>("SstWriter", "Engine", "ThrowUp",
                                         "Engine does not support NotifyEngineAttribute");
}

void SstWriter::NotifyEngineAttribute(std::string name, AttributeBase *Attr, void *data) noexcept
{
    m_BP5Serializer.OnetimeMarshalAttribute(*Attr);
}

void SstWriter::EndStep()
{
    PERFSTUBS_SCOPED_TIMER_FUNC();
    if (!m_BetweenStepPairs)
    {
        helper::Throw<std::logic_error>("Engine", "SstWriter", "EndStep",
                                        "EndStep() is called without a successful BeginStep()");
    }
    m_BetweenStepPairs = false;
    if (m_WriterDefinitionsLocked && !m_DefinitionsNotified)
    {
        SstWriterDefinitionLock(m_Output, m_WriterStep);
        m_DefinitionsNotified = true;
    }
    format::BP5Serializer::TimestepInfo *TSInfo =
        new format::BP5Serializer::TimestepInfo(m_BP5Serializer.CloseTimestep(m_WriterStep));
    auto lf_FreeBlocks = [](void *vBlock) {
        BP5DataBlock *BlockToFree = reinterpret_cast<BP5DataBlock *>(vBlock);
        //  Free data and metadata blocks here.  BlockToFree is the newblock
        //  value in the enclosing function.
        free(BlockToFree->MetaMetaBlocks);
        delete BlockToFree->TSInfo->DataBuffer;
        delete BlockToFree->TSInfo;
        delete BlockToFree;
    };

    // Only rank 0 sends formats and attributes to readers.  Gather just the
    // distinct ones there; other ranks pass none.
    std::vector<format::BP5Base::MetaMetaInfoBlock> UniqueMetaMetaBlocks =
        TSInfo->NewMetaMetaBlocks;
    std::vector<core::iovec> AttributeBlocks;
    if (TSInfo->AttributeEncodeBuffer && TSInfo->AttributeEncodeBuffer->m_FixedSize)
    {
        AttributeBlocks.push_back(
            {TSInfo->AttributeEncodeBuffer->Data(), TSInfo->AttributeEncodeBuffer->m_FixedSize});
    }
    std::vector<size_t> MetaEncodeSize = {TSInfo->MetaEncodeBuffer->m_FixedSize};
    std::vector<uint64_t> WriterDataPositions = {0};
    format::BP5Helper::BP5AggregateInformation(m_Comm, m_Profiler, UniqueMetaMetaBlocks,
                                               AttributeBlocks, MetaEncodeSize,
                                               WriterDataPositions);
    if (m_Comm.Rank() != 0)
    {
        UniqueMetaMetaBlocks.clear();
        AttributeBlocks.clear();
    }

    BP5DataBlock *newblock = new BP5DataBlock;
    SstMetaMetaList MetaMetaBlocks =
        (SstMetaMetaList)malloc((UniqueMetaMetaBlocks.size() + 1) * sizeof(MetaMetaBlocks[0]));
    int i = 0;
    for (const auto &MM : UniqueMetaMetaBlocks)
    {
        MetaMetaBlocks[i].BlockData = MM.MetaMetaInfo;
        MetaMetaBlocks[i].BlockSize = MM.MetaMetaInfoLen;
        MetaMetaBlocks[i].ID = MM.MetaMetaID;
        MetaMetaBlocks[i].IDSize = MM.MetaMetaIDLen;
        i++;
    }
    MetaMetaBlocks[UniqueMetaMetaBlocks.size()] = {NULL, 0, NULL, 0};
    newblock->MetaMetaBlocks = MetaMetaBlocks;
    newblock->metadata.DataSize = TSInfo->MetaEncodeBuffer->m_FixedSize;
    newblock->metadata.block = TSInfo->MetaEncodeBuffer->Data();
    std::vector<core::iovec> iovec = TSInfo->DataBuffer->DataVec();
    if (!iovec.empty())
    {
        newblock->data.DataSize = iovec[0].iov_len;
        newblock->data.block = (char *)iovec[0].iov_base;
    }
    else
    {
        newblock->data.DataSize = 0;
        newblock->data.block = nullptr;
    }
    newblock->TSInfo = TSInfo;
    std::vector<_SstData> AttributeData;
    for (const auto &A : AttributeBlocks)
    {
        AttributeData.push_back({A.iov_len, (char *)A.iov_base});
    }
    SstProvideTimestepMM(m_Output, &newblock->metadata, &newblock->data, m_WriterStep,
                         lf_FreeBlocks, newblock, AttributeData.data(), (int)AttributeData.size(),
                         NULL, newblock, MetaMetaBlocks);
    // On rank 0 the distinct blocks are copies; cp has encoded them by now.
    if (m_Comm.Rank() == 0)
    {
        for (auto &MM : UniqueMetaMetaBlocks)
        {
            free(MM.MetaMetaInfo);
            free(MM.MetaMetaID);
        }
        for (auto &A : AttributeBlocks)
        {
            free((void *)A.iov_base);
        }
    }
}

void SstWriter::PerformPuts() {}

void SstWriter::Flush(const int transportIndex) {}

// PRIVATE functions below
void SstWriter::Init()
{
    SstParamParser Parser;

    Parser.ParseParams(m_IO, Params, m_UserOptions);

    if (Params.verbose < 0 || Params.verbose > 5)
    {
        helper::Throw<std::invalid_argument>("Engine", "SstWriter", "Init",
                                             "ERROR: Method verbose argument must be an "
                                             "integer in the range [0,5], in call to "
                                             "Open or Engine constructor\n");
    }
    m_BP5Serializer.m_StatsLevel = Params.StatsLevel;
    m_BP5Serializer.m_Engine = this;
    // attributes defined before Open; later ones arrive via NotifyEngineAttribute
    for (const auto &attributePair : m_IO.GetAttributes())
        m_BP5Serializer.OnetimeMarshalAttribute(*(attributePair.second));
}

#define declare_type(T)                                                                            \
    void SstWriter::DoPutSync(Variable<T> &variable, const T *values)                              \
    {                                                                                              \
        PutSyncCommon(variable, values);                                                           \
    }                                                                                              \
    void SstWriter::DoPutDeferred(Variable<T> &variable, const T *values)                          \
    {                                                                                              \
        PutSyncCommon(variable, values);                                                           \
    }

ADIOS2_FOREACH_STDTYPE_1ARG(declare_type)
#undef declare_type

void SstWriter::PutStructCommon(VariableBase &variable, const void *data)
{
    size_t *Shape = NULL;
    size_t *Start = NULL;
    size_t *Count = NULL;
    size_t DimCount = 0;

    if (m_BetweenStepPairs == false)
    {
        helper::Throw<std::logic_error>("Engine", "SstWriter", "PutSyncCommon",
                                        "When using the SST engine in ADIOS2, "
                                        "Put() calls must appear between "
                                        "BeginStep/EndStep pairs");
    }

    if (variable.m_ShapeID == ShapeID::GlobalArray)
    {
        DimCount = variable.m_Shape.size();
        Shape = variable.m_Shape.data();
        Start = variable.m_Start.data();
        Count = variable.m_Count.data();
    }
    else if (variable.m_ShapeID == ShapeID::LocalArray)
    {
        DimCount = variable.m_Count.size();
        Count = variable.m_Count.data();
    }
    m_BP5Serializer.Marshal((void *)&variable, variable.m_Name.c_str(), variable.m_Type,
                            variable.m_ElementSize, DimCount, Shape, Count, Start, data, true,
                            nullptr);
}

void SstWriter::DoPutStructSync(VariableStruct &variable, const void *data)
{
    PutStructCommon(variable, data);
}

void SstWriter::DoPutStructDeferred(VariableStruct &variable, const void *data)
{
    PutStructCommon(variable, data);
}

void PutStruct(VariableStruct &, const void *, bool);

void SstWriter::DoClose(const int transportIndex) { SstWriterClose(m_Output); }

/**
 * Called if destructor is called on an open engine.  Should warn or take any
 * non-complex measure that might help recover.
 */
void SstWriter::DestructorClose(bool Verbose) noexcept
{
    if (Verbose)
    {
        std::cerr << "SST Writer \"" << m_Name << "\" Destroyed without a prior Close()."
                  << std::endl;
        std::cerr << "This may result in loss of data and/or disconnect "
                     "warnings for a connected SST Reader."
                  << std::endl;
    }
    m_IsOpen = false;
    // should at least call control plane to remove contact file
}

} // end namespace engine
} // end namespace core
} // end namespace adios2
