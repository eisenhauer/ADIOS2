/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BufferV.cpp
 *
 */

#include "MallocV.h"
#include "adios2/toolkit/format/buffer/BufferV.h"
#include <string.h>

namespace adios2
{
namespace format
{

MallocV::MallocV(const std::string type, const bool AlwaysCopy,
                 size_t InitialBufferSize, double GrowthFactor)
: BufferV(type, AlwaysCopy), m_InitialBufferSize(InitialBufferSize),
  m_GrowthFactor(GrowthFactor)
{
}

MallocV::~MallocV()
{
    if (m_InternalBlock)
        free(m_InternalBlock);
}

void MallocV::Reset()
{
    CurOffset = 0;
    m_internalPos = 0;
    DataV.clear();
}

size_t MallocV::AddToVec(const size_t size, const void *buf, int align,
                         bool CopyReqd)
{
    int badAlign = CurOffset % align;
    if (badAlign)
    {
        int addAlign = align - badAlign;
        char zero[16] = {0};
        AddToVec(addAlign, zero, 1, true);
    }
    size_t retOffset = CurOffset;

    if (size == 0)
        return CurOffset;

    if (!CopyReqd && !m_AlwaysCopy)
    {
        // just add buf to internal version of output vector
        VecEntry entry = {true, buf, 0, size};
        DataV.push_back(entry);
    }
    else
    {
        if (m_internalPos + size > m_AllocatedSize)
        {
            // need to resize
            size_t NewSize;
            if (m_internalPos + size > m_AllocatedSize * m_GrowthFactor)
            {
                // just grow as needed (more than GrowthFactor)
                NewSize = m_internalPos + size;
            }
            else
            {
                NewSize = (size_t)(m_AllocatedSize * m_GrowthFactor);
            }
            m_InternalBlock = (char *)realloc(m_InternalBlock, NewSize);
            m_AllocatedSize = NewSize;
        }
        memcpy(m_InternalBlock + m_internalPos, buf, size);

        if (DataV.size() && !DataV.back().External &&
            (m_internalPos == (DataV.back().Offset + DataV.back().Size)))
        {
            // just add to the size of the existing tail entry
            DataV.back().Size += size;
        }
        else
        {
            DataV.push_back({false, NULL, m_internalPos, size});
        }
        m_internalPos += size;
    }
    CurOffset = retOffset + size;
    return retOffset;
}

MallocV::BufferV_iovec MallocV::DataVec() noexcept
{
    BufferV_iovec ret = new iovec[DataV.size() + 1];
    for (std::size_t i = 0; i < DataV.size(); ++i)
    {
        if (DataV[i].External)
        {
            ret[i].iov_base = DataV[i].Base;
        }
        else
        {
            ret[i].iov_base = m_InternalBlock + DataV[i].Offset;
        }
        ret[i].iov_len = DataV[i].Size;
    }
    ret[DataV.size()] = {NULL, 0};
    return ret;
}

} // end namespace format
} // end namespace adios2