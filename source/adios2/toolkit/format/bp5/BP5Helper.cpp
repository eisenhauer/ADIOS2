/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BP5Helper.cpp
 */

#include "BP5Helper.h"
#include "adios2/helper/adiosFunctions.h"
#include <adios2sys/MD5.h> // Include the MD5 header
#include <iomanip>         // put_time

#include "fm.h"

#ifdef _WIN32
#pragma warning(disable : 4250)
#endif

namespace adios2
{
namespace format
{

BP5Helper::digest BP5Helper::HashOfBlock(const void *block, const size_t block_len)
{
    adios2sysMD5 *md5 = adios2sysMD5_New();
    if (!md5)
    {
        throw std::runtime_error("Failed to create MD5 instance");
    }

    // Initialize the MD5 instance
    adios2sysMD5_Initialize(md5);

    // Update the MD5 instance with the input data
    adios2sysMD5_Append(md5, reinterpret_cast<const unsigned char *>(block), block_len);

    // Finalize the MD5 digest and get the hash value
    BP5Helper::digest ret;
    adios2sysMD5_Finalize(md5, &ret.x[0]);

    // Clean up the MD5 instance
    adios2sysMD5_Delete(md5);

    return ret;
}

std::vector<char>
BP5Helper::BuildNodeContrib(const digest attrHash, const size_t attrSize,
                            const std::vector<BP5Base::MetaMetaInfoBlock> MMBlocks,
                            const size_t MetaEncodeSize,
                            const std::vector<uint64_t> WriterDataPositions)
{
    std::vector<char> ret;
    size_t MMBlocksSize = MMBlocks.size();
    size_t len = sizeof(digest) + 2 * sizeof(size_t) +
                 MMBlocksSize * (sizeof(size_t) + sizeof(digest)) + sizeof(size_t) +
                 sizeof(uint64_t);
    ret.resize(len);
    size_t position = 0;
    helper::CopyToBuffer(ret, position, attrHash.x, sizeof(digest));
    helper::CopyToBuffer(ret, position, &attrSize, 1);
    helper::CopyToBuffer(ret, position, &MMBlocksSize, 1);
    for (auto &MM : MMBlocks)
    {
        digest D;
        std::memset(&D.x[0], 0, sizeof(digest));
        std::memcpy(&D.x[0], MM.MetaMetaID, MM.MetaMetaIDLen);
        helper::CopyToBuffer(ret, position, D.x, sizeof(digest));
        size_t AlignedSize = ((MM.MetaMetaInfoLen + 7) & ~0x7);
        helper::CopyToBuffer(ret, position, &AlignedSize, 1);
    }
    helper::CopyToBuffer(ret, position, &MetaEncodeSize, 1);
    helper::CopyToBuffer(ret, position, &WriterDataPositions[0], 1);
    return ret;
}

std::vector<char>
BP5Helper::BuildFixedNodeContrib(const digest attrHash, const size_t attrSize,
                                 const std::vector<BP5Base::MetaMetaInfoBlock> MMBlocks,
                                 const size_t MetaEncodeSize,
                                 const std::vector<uint64_t> WriterDataPositions)
{
    std::vector<char> ret;
    size_t MMBlocksSize = MMBlocks.size();
    size_t len = sizeof(node_contrib);
    ret.resize(len);
    auto NC = reinterpret_cast<node_contrib *>(ret.data());
    NC->AttrHash = attrHash;
    NC->AttrSize = attrSize;
    NC->MMBCount = MMBlocks.size();
    for (size_t i = 0; i < FIXED_MMB_SLOT_COUNT; i++)
    {
        std::memset(&NC->MMBArray[i].x[0], 0, sizeof(digest));
        auto MM = &MMBlocks[i];
        if (i < MMBlocks.size())
        {
            std::memcpy(&NC->MMBArray[i].x[0], MM->MetaMetaID, MM->MetaMetaIDLen);
            size_t AlignedSize = ((MM->MetaMetaInfoLen + 7) & ~0x7);
            NC->MMBSizeArray[i] = AlignedSize;
        }
        else
        {
            NC->MMBSizeArray[i] = 0;
        }
    }
    NC->MetaEncodeSize = MetaEncodeSize;
    NC->WriterDataPosition = WriterDataPositions[0];
    return ret;
}

void BP5Helper::BreakdownFixedIncomingMInfo(
    const size_t NodeCount, const std::vector<char> RecvBuffer,
    std::vector<size_t> &SecondRecvCounts, std::vector<uint64_t> &BcastInfo,
    std::vector<uint64_t> &WriterDataPositions, std::vector<size_t> &MetaEncodeSize,
    std::vector<size_t> &AttrSizes, std::vector<size_t> &MMBSizes, std::vector<digest> &MMBIDs)
{
    auto lf_digestZero = [](const digest D) -> bool {
        return ((D.x[0] == 0) && (std::memcmp(&D.x[0], &D.x[1], (sizeof(D.x) - 1)) == 0));
    };

    std::set<digest> AttrSet;
    std::set<digest> MMBSet;
    MetaEncodeSize.resize(NodeCount);
    WriterDataPositions.resize(NodeCount);
    SecondRecvCounts.resize(NodeCount);
    BcastInfo.resize(NodeCount);
    AttrSizes.resize(NodeCount);
    const node_contrib *NCArray = reinterpret_cast<const node_contrib *>(RecvBuffer.data());
    for (size_t node = 0; node < NodeCount; node++)
    {
        const node_contrib *NC = &NCArray[node];
        digest thisAttrHash;
        bool needAttr = false;
        size_t MMBlockCount;
        size_t SecondRecvSize = 0;
        thisAttrHash = NC->AttrHash;
        size_t AttrSize = NC->AttrSize;
        AttrSizes[node] = AttrSize;
        if (AttrSize && !AttrSet.count(thisAttrHash))
        {
            AttrSet.insert(thisAttrHash);
            needAttr = true;
            size_t AlignedSize = ((AttrSize + 7) & ~0x7);
            SecondRecvSize += AlignedSize;
        }

        size_t MMsNeeded = 0;
        MMBlockCount = NC->MMBCount;
        if (MMBlockCount > FIXED_MMB_SLOT_COUNT)
        {
            BcastInfo[0] = (size_t)-1;
            // we can't finish this, fallback
            return;
        }
        for (size_t block = 0; block < MMBlockCount; block++)
        {
            digest thisMMB = NC->MMBArray[block];
            size_t thisMMBSize = NC->MMBSizeArray[block];
            if (!MMBSet.count(thisMMB))
            {
                MMBSet.insert(thisMMB);
                MMsNeeded += (1 << block);
                size_t AlignedSize = ((thisMMBSize + 7) & ~0x7);
                MMBSizes.push_back(AlignedSize);
                MMBIDs.push_back(thisMMB);
                SecondRecvSize += AlignedSize;
            }
        }
        MetaEncodeSize[node] = NC->MetaEncodeSize;
        size_t WDP = NC->WriterDataPosition;
        WriterDataPositions[node] = WDP;
        SecondRecvCounts[node] = SecondRecvSize;
        BcastInfo[node] = needAttr ? ((uint64_t)1 << 63) : 0;
        BcastInfo[node] |= MMsNeeded;
    }
}

void BP5Helper::BreakdownIncomingMInfo(
    const std::vector<size_t> RecvCounts, const std::vector<char> RecvBuffer,
    std::vector<size_t> &SecondRecvCounts, std::vector<uint64_t> &BcastInfo,
    std::vector<uint64_t> &WriterDataPositions, std::vector<size_t> &MetaEncodeSize,
    std::vector<size_t> &AttrSizes, std::vector<size_t> &MMBSizes, std::vector<digest> &MMBIDs)
{
    auto lf_digestZero = [](const digest D) -> bool {
        return ((D.x[0] == 0) && (std::memcmp(&D.x[0], &D.x[1], (sizeof(D.x) - 1)) == 0));
    };

    std::set<digest> AttrSet;
    std::set<digest> MMBSet;
    MetaEncodeSize.resize(RecvCounts.size());
    WriterDataPositions.resize(RecvCounts.size());
    SecondRecvCounts.resize(RecvCounts.size());
    BcastInfo.resize(RecvCounts.size());
    AttrSizes.resize(RecvCounts.size());
    size_t pos = 0, sum = 0;
    for (size_t node = 0; node < RecvCounts.size(); node++)
    {
        digest thisAttrHash;
        bool needAttr = false;
        size_t MMBlockCount;
        size_t SecondRecvSize = 0;
        helper::ReadArray(RecvBuffer, pos, thisAttrHash.x, sizeof(thisAttrHash.x), false);
        size_t AttrSize = helper::ReadValue<size_t>(RecvBuffer, pos, false);
        AttrSizes[node] = AttrSize;
        if (AttrSize && !AttrSet.count(thisAttrHash))
        {
            AttrSet.insert(thisAttrHash);
            needAttr = true;
            size_t AlignedSize = ((AttrSize + 7) & ~0x7);
            SecondRecvSize += AlignedSize;
        }

        size_t MMsNeeded = 0;
        MMBlockCount = helper::ReadValue<size_t>(RecvBuffer, pos, false);
        for (size_t block = 0; block < MMBlockCount; block++)
        {
            digest thisMMB;
            helper::ReadArray(RecvBuffer, pos, thisMMB.x, sizeof(thisMMB.x), false);
            size_t thisMMBSize = helper::ReadValue<size_t>(RecvBuffer, pos, false);
            if (!MMBSet.count(thisMMB))
            {
                MMBSet.insert(thisMMB);
                MMsNeeded += (1 << block);
                size_t AlignedSize = ((thisMMBSize + 7) & ~0x7);
                MMBSizes.push_back(AlignedSize);
                MMBIDs.push_back(thisMMB);
                SecondRecvSize += AlignedSize;
            }
        }
        MetaEncodeSize[node] = helper::ReadValue<size_t>(RecvBuffer, pos, false);
        size_t WDP = helper::ReadValue<size_t>(RecvBuffer, pos, false);
        WriterDataPositions[node] = WDP;
        SecondRecvCounts[node] = SecondRecvSize;
        BcastInfo[node] = needAttr ? ((uint64_t)1 << 63) : 0;
        BcastInfo[node] |= MMsNeeded;

        // end of loop check
        sum += RecvCounts[node];
        if (pos != sum)
            throw std::logic_error("Bad deserialization");
    }
}

void BP5Helper::BreakdownIncomingMData(const std::vector<size_t> &RecvCounts,
                                       std::vector<uint64_t> &BcastInfo,
                                       const std::vector<char> &IncomingMMA,
                                       std::vector<BP5Base::MetaMetaInfoBlock> &NewMetaMetaBlocks,
                                       std::vector<core::iovec> &AttributeEncodeBuffers,
                                       std::vector<size_t> AttrSize, std::vector<size_t> MMBSizes,
                                       std::vector<digest> MMBIDs)
{
    size_t pos = 0, sum = 0;
    AttributeEncodeBuffers.clear();
    NewMetaMetaBlocks.clear();
    for (int node = 0; node < RecvCounts.size(); node++)
    {
        if (BcastInfo[node] & ((uint64_t)1 << 63))
        {
            void *buffer = malloc(AttrSize[node]);
            helper::ReadArray(IncomingMMA, pos, (char *)buffer, AttrSize[node]);
            AttributeEncodeBuffers.push_back({buffer, AttrSize[node]});
            BcastInfo[node] &= ~((uint64_t)1 << 63);
        }
        size_t b = 0;
        while (BcastInfo[node])
        {
            if (BcastInfo[node] & ((uint64_t)1 << b))
            {
                MetaMetaInfoBlock mmib;
                size_t index = NewMetaMetaBlocks.size();
                mmib.MetaMetaInfoLen = MMBSizes[index];
                mmib.MetaMetaInfo = (char *)malloc(MMBSizes[index]);
                helper::ReadArray(IncomingMMA, pos, mmib.MetaMetaInfo, mmib.MetaMetaInfoLen);
                mmib.MetaMetaIDLen = FMformatID_len((char *)&MMBIDs[index]);
                mmib.MetaMetaID = (char *)malloc(mmib.MetaMetaIDLen);
                std::memcpy(mmib.MetaMetaID, (char *)&MMBIDs[index], mmib.MetaMetaIDLen);
                NewMetaMetaBlocks.push_back(mmib);
                BcastInfo[node] &= ~((uint64_t)1 << b);
                b++;
            }
        }
        // end of loop check
        sum += RecvCounts[node];
        if (pos != sum)
            throw std::logic_error("Bad deserialization");
    }
}

// clang-format off
/*
 *  BP5AggregateInformation
 *
 *  Here we want to avoid some of the problems with prior approaches
 *  to metadata aggregation by being more selective up front, possibly
 *  at the cost of involving more collective operations but hopefully
 *  with smaller data sizes.  In particular, in a first phase we're
 *  only aggreggating MetaMetadata IDs (not bodies), and the hashes of
 *  attribute blocks.  This is has more back-and-forth than prior
 *  methods, but it avoids moving duplicate attributes or metametadata
 *  blocks.
 *
 *
 * Basic protocol of this aggregation method:
 * First 3 steps below occur in the helper function
 * 1) Fixed-size (144 bytes) gather
 *   This gathers AttrHash, AttrSize, MetaDataEncodeSize,
 *   WriterDataPosition, MetaMetaBlockCount and first 4
 *   MetaMetaBlockIDs
 * 
 * 2) Fixed-size (8 byte) bcast
 *   This Broadcasts a bitmap of what attributes and metametablocks
 *   are unique and required at rank 0
 * 
 * IF some rank has more than 4 MetaMetaBlocks, we know after the
 * gather above and an indicator is included in the bcast so that all
 * ranks fallback to a dynamic gather that includes all the
 * metametablock IDs.  This should be VERY rare (only on the first
 * timestep after multiple new structured types are defined).
 *    <exception protocol if lots of unique metametadata>
 *      2a) Fixed-size (8 byte) size gather
 *      2b) Variable-sized gatherv
 *      2c) Fixed-size (8 byte) bcast
 * 
 * IF there are new Attribute blocks or new MetaMetaBlocks Step 3
 * gathers them.  This may never happen after timestep 0
 * 
 * 3) Variable-size gatherv to gather new Attrs or MetaMetaBlocks
 * 
 * The last step (4) occurs in the engine, not the helper function,
 * but size information is provided by the helper
 * 4) variable sized gather of the actual metadata blocks
 * 
 */
// clang-format on

void BP5Helper::BP5AggregateInformation(helper::Comm &mpiComm,
                                        adios2::profiling::JSONProfiler &Profiler,
                                        std::vector<BP5Base::MetaMetaInfoBlock> &NewMetaMetaBlocks,
                                        std::vector<core::iovec> &AttributeEncodeBuffers,
                                        std::vector<size_t> &MetaEncodeSize,
                                        std::vector<uint64_t> &WriterDataPositions)
{

    // Incoming, we expect a single Attribute Encode buffer, get its hash
    BP5Helper::digest attrHash;
    size_t attrLen = 0;
    memset(&attrHash, 0, sizeof(attrHash));
    if (AttributeEncodeBuffers.size() > 0)
    {
        size_t AlignedSize = ((AttributeEncodeBuffers[0].iov_len + 7) & ~0x7);
        attrLen = AlignedSize;
    }
    if (attrLen > 0)
        attrHash = HashOfBlock(AttributeEncodeBuffers[0].iov_base, attrLen);
    std::vector<char> RecvBuffer;
    std::vector<uint64_t> BcastInfo;
    std::vector<size_t> SecondRecvCounts;
    std::vector<size_t> AttrSize;
    std::vector<size_t> MMBSizes;
    std::vector<digest> MMBIDs;
    auto myFixedContrib = BuildFixedNodeContrib(attrHash, attrLen, NewMetaMetaBlocks,
                                                MetaEncodeSize[0], WriterDataPositions);
    bool NeedDynamic = false;

    if (mpiComm.Rank() == 0)
    {
        RecvBuffer.resize(mpiComm.Size() * sizeof(node_contrib));
        std::time_t t = std::time(nullptr);
        //        std::cout << "start first gather arrays [" << std::put_time(std::localtime(&t),
        //        "%F %T %Z")
        //                  << "] " << std::endl;
        Profiler.Start("FixedMetaInfoGather");
        mpiComm.GatherArrays(myFixedContrib.data(), myFixedContrib.size(), RecvBuffer.data(), 0);
        Profiler.Stop("FixedMetaInfoGather");
        t = std::time(nullptr);
        //        std::cout << "end first gather arrays [" << std::put_time(std::localtime(&t), "%F
        //        %T %Z")
        //                  << "] " << std::endl;
        BreakdownFixedIncomingMInfo(mpiComm.Size(), RecvBuffer, SecondRecvCounts, BcastInfo,
                                    WriterDataPositions, MetaEncodeSize, AttrSize, MMBSizes,
                                    MMBIDs);
        t = std::time(nullptr);
        //        std::cout << "start bcst [" << std::put_time(std::localtime(&t), "%F %T %Z") << "]
        //        "
        //                  << std::endl;
        Profiler.Start("MetaInfoBcast");
        mpiComm.Bcast(BcastInfo.data(), BcastInfo.size(), 0, "");
        Profiler.Stop("MetaInfoBcast");
        t = std::time(nullptr);
        //        std::cout << "end bcst [" << std::put_time(std::localtime(&t), "%F %T %Z") << "] "
        //                  << std::endl;
    }
    else
    {
        Profiler.Start("FixedMetaInfoGather");
        mpiComm.GatherArrays(myFixedContrib.data(), myFixedContrib.size(), RecvBuffer.data(), 0);
        Profiler.Stop("FixedMetaInfoGather");
        BcastInfo.resize(mpiComm.Size());
        Profiler.Start("MetaInfoBcast");
        mpiComm.Bcast(BcastInfo.data(), BcastInfo.size(), 0, "");
        Profiler.Stop("MetaInfoBcast");
    }

    NeedDynamic = BcastInfo[0] == (size_t)-1;
    if (NeedDynamic)
    {
        /*
         * This portion only happens if some node has more than FIXED_MBB_SLOT_COUNT metameta blocks
         * which should be almost never in practice.
         */
        auto myContrib = BuildNodeContrib(attrHash, attrLen, NewMetaMetaBlocks, MetaEncodeSize[0],
                                          WriterDataPositions);
        std::vector<size_t> RecvCounts = mpiComm.GatherValues(myContrib.size(), 0);

        if (mpiComm.Rank() == 0)
        {
            uint64_t TotalSize = 0;
            std::time_t t = std::time(nullptr);
            //            std::cout << "in dynamic  values [" << std::put_time(std::localtime(&t),
            //            "%F %T %Z")
            //                      << "] " << std::endl;
            TotalSize = std::accumulate(RecvCounts.begin(), RecvCounts.end(), size_t(0));
            RecvBuffer.resize(TotalSize);
            mpiComm.GathervArrays(myContrib.data(), myContrib.size(), RecvCounts.data(),
                                  RecvCounts.size(), RecvBuffer.data(), 0);
            t = std::time(nullptr);
            //            std::cout << "enddynamic gather arrayss ["
            //                      << std::put_time(std::localtime(&t), "%F %T %Z") << "] " <<
            //                      std::endl;
            BreakdownIncomingMInfo(RecvCounts, RecvBuffer, SecondRecvCounts, BcastInfo,
                                   WriterDataPositions, MetaEncodeSize, AttrSize, MMBSizes, MMBIDs);
            mpiComm.Bcast(BcastInfo.data(), BcastInfo.size(), 0, "");
            t = std::time(nullptr);
            //            std::cout << "enddynamic bcase [" << std::put_time(std::localtime(&t), "%F
            //            %T %Z")
            //                      << "] " << std::endl;
        }
        else
        {
            mpiComm.GathervArrays(myContrib.data(), myContrib.size(), RecvCounts.data(),
                                  RecvCounts.size(), RecvBuffer.data(), 0);
            BcastInfo.resize(mpiComm.Size());
            mpiComm.Bcast(BcastInfo.data(), BcastInfo.size(), 0, "");
        }
    }
    uint64_t MMASummary = std::accumulate(BcastInfo.begin(), BcastInfo.end(), size_t(0));

    if (MMASummary == 0)
    {
        // Nobody has anything new to contribute WRT attributes or metametadata
        // All mpiranks have the same info and will make the same decision
        if (mpiComm.Rank() == 0)
        {
            std::time_t t = std::time(nullptr);
            //            std::cout << "aggregate shortciruit [" <<
            //            std::put_time(std::localtime(&t), "%F %T %Z")
            //                      << "] " << std::endl;
        }
        return;
    }
    // assemble my contribution to mm and attr gather
    std::vector<char> myMMAcontrib;
    size_t pos = 0;
    if (BcastInfo[mpiComm.Rank()] & ((uint64_t)1 << 63))
    {
        // Need attr block
        size_t AlignedSize = ((AttributeEncodeBuffers[0].iov_len + 7) & ~0x7);
        size_t pad = AlignedSize - AttributeEncodeBuffers[0].iov_len;
        myMMAcontrib.resize(AlignedSize);
        helper::CopyToBuffer(myMMAcontrib, pos, (char *)AttributeEncodeBuffers[0].iov_base,
                             AttributeEncodeBuffers[0].iov_len);
        if (pad)
        {
            uint64_t zero = 0;
            helper::CopyToBuffer(myMMAcontrib, pos, (char *)&zero, pad);
        }
    }
    for (size_t b = 0; b < NewMetaMetaBlocks.size(); b++)
    {
        if (BcastInfo[mpiComm.Rank()] & (1 << b))
        {
            size_t AlignedSize = ((NewMetaMetaBlocks[b].MetaMetaInfoLen + 7) & ~0x7);
            size_t pad = AlignedSize - NewMetaMetaBlocks[b].MetaMetaInfoLen;
            myMMAcontrib.resize(pos + AlignedSize);
            helper::CopyToBuffer(myMMAcontrib, pos, NewMetaMetaBlocks[b].MetaMetaInfo,
                                 NewMetaMetaBlocks[b].MetaMetaInfoLen);
            if (pad)
            {
                uint64_t zero = 0;
                helper::CopyToBuffer(myMMAcontrib, pos, (char *)&zero, pad);
            }
        }
    }
    // per above, is 8-byte aligned
    uint64_t AlignedContribCount = myMMAcontrib.size() / 8;
    uint64_t *AlignedContrib = reinterpret_cast<uint64_t *>(myMMAcontrib.data());
    uint64_t TotalSize =
        std::accumulate(SecondRecvCounts.begin(), SecondRecvCounts.end(), size_t(0));
    uint64_t AlignedTotalSize = TotalSize / 8;
    auto AlignedCounts = SecondRecvCounts;
    for (auto &C : AlignedCounts)
        C /= 8;
    if (mpiComm.Rank() == 0)
    {
        std::vector<char> IncomingMMA(TotalSize);
        uint64_t *AlignedIncomingData = reinterpret_cast<uint64_t *>(IncomingMMA.data());
        std::time_t t = std::time(nullptr);
        //        std::cout << "begin MMA gather [" << std::put_time(std::localtime(&t), "%F %T %Z")
        //        << "] "
        //                  << std::endl;
        Profiler.Start("SelectMetaInfoGather");
        mpiComm.GathervArrays(AlignedContrib, AlignedContribCount, AlignedCounts.data(),
                              AlignedCounts.size(), AlignedIncomingData, 0);
        Profiler.Stop("SelectMetaInfoGather");
        t = std::time(nullptr);
        //        std::cout << "endn MMA gather [" << std::put_time(std::localtime(&t), "%F %T %Z")
        //        << "] "
        //                  << std::endl;
        BreakdownIncomingMData(SecondRecvCounts, BcastInfo, IncomingMMA, NewMetaMetaBlocks,
                               AttributeEncodeBuffers, AttrSize, MMBSizes, MMBIDs);
    }
    else
    {
        Profiler.Start("SelectMetaInfoGather");
        mpiComm.GathervArrays(AlignedContrib, AlignedContribCount, AlignedCounts.data(),
                              AlignedCounts.size(), (uint64_t *)nullptr, 0);
        Profiler.Stop("SelectMetaInfoGather");
    }
}

} // namespace format
} // namespace adios
