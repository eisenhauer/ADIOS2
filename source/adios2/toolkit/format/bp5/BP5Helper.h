/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BP5Helper.h
 */

#ifndef ADIOS2_TOOLKIT_FORMAT_BP5_BP5HELPER_H_
#define ADIOS2_TOOLKIT_FORMAT_BP5_BP5HELPER_H_

#include "BP5Base.h"
#include "adios2/core/Attribute.h"
#include "adios2/core/CoreTypes.h"
#include "adios2/toolkit/profiling/iochrono/IOChrono.h"
#ifdef _WIN32
#pragma warning(disable : 4250)
#endif

namespace adios2
{
namespace format
{

class BP5Helper : virtual public BP5Base
{
public:
    static void BP5AggregateInformation(helper::Comm &mpiComm,
                                        adios2::profiling::JSONProfiler &Profiler,
                                        std::vector<BP5Base::MetaMetaInfoBlock> &NewMetaMetaBlocks,
                                        std::vector<core::iovec> &AttributeEncodeBuffers,
                                        std::vector<size_t> &MetaEncodeSize,
                                        std::vector<uint64_t> &WriterDataPositions);

    struct digest
    {
        unsigned char x[16];
        // compare for order
        bool operator<(const digest &dg) const { return (memcmp(&x[0], &dg.x[0], sizeof(x))); }
    };
    static digest HashOfBlock(const void *block, const size_t block_len);
private:
#define FIXED_MMB_SLOT_COUNT 4
    struct node_contrib
    {
        digest AttrHash;
        size_t AttrSize;
        size_t MMBCount;
        digest MMBArray[FIXED_MMB_SLOT_COUNT];
        size_t MMBSizeArray[FIXED_MMB_SLOT_COUNT];
        size_t MetaEncodeSize;
        uint64_t WriterDataPosition;
    };
    static std::vector<char>
    BuildNodeContrib(const digest attrHash, size_t attrSize,
                     const std::vector<BP5Base::MetaMetaInfoBlock> NewMetaMetaBlocks,
                     const size_t MetaEncodeSize, const std::vector<uint64_t> WriterDataPositions);
    static std::vector<char>
    BuildFixedNodeContrib(const digest attrHash, size_t attrSize,
                          const std::vector<BP5Base::MetaMetaInfoBlock> NewMetaMetaBlocks,
                          const size_t MetaEncodeSize,
                          const std::vector<uint64_t> WriterDataPositions);
    static void
    BreakdownIncomingMInfo(const std::vector<size_t> RecvCounts, const std::vector<char> RecvBuffer,
                           std::vector<size_t> &SecondRecvCounts, std::vector<uint64_t> &BcastInfo,
                           std::vector<uint64_t> &WriterDataPositions,
                           std::vector<size_t> &MetaEncodeSize, std::vector<size_t> &AttrSizes,
                           std::vector<size_t> &MMBSizes, std::vector<digest> &MBBIDs);
    static void BreakdownFixedIncomingMInfo(
        const size_t NodeCount, const std::vector<char> RecvBuffer,
        std::vector<size_t> &SecondRecvCounts, std::vector<uint64_t> &BcastInfo,
        std::vector<uint64_t> &WriterDataPositions, std::vector<size_t> &MetaEncodeSize,
        std::vector<size_t> &AttrSizes, std::vector<size_t> &MMBSizes, std::vector<digest> &MBBIDs);
    static void BreakdownIncomingMData(const std::vector<size_t> &RecvCounts,
                                       std::vector<uint64_t> &BcastInfo,
                                       const std::vector<char> &IncomingMMA,
                                       std::vector<BP5Base::MetaMetaInfoBlock> &NewMetaMetaBlocks,
                                       std::vector<core::iovec> &AttributeEncodeBuffers,
                                       std::vector<size_t> AttrSize, std::vector<size_t> MMBSizes,
                                       std::vector<digest> MMBIDs);
};
} // end namespace format
} // end namespace adios2

#endif /*  ADIOS2_TOOLKIT_FORMAT_BP5_BP5HELPER_H_ */
