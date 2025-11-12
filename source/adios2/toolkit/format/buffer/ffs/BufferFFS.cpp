/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BufferFFS.cpp
 *
 */

#include "BufferFFS.h"
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace adios2
{
namespace format
{

BufferFFS::BufferFFS(FFSBuffer Buf, void *data, size_t len) : Buffer("BufferFFS", len)
{
    std::cout << "Buffer " << (void*)this << " FFS created with Buf = " << (void *)Buf << "  data = " << data
              << std::endl;
    m_buffer = Buf;
    m_data = data;
}

BufferFFS::~BufferFFS()
{
    std::cout << "Freeing " << (void*)this << " Buffer FFS  = " << (void *)m_buffer << std::endl;
    free_FFSBuffer(m_buffer);
    std::cout << "Done Freeing " << (void*)this << " Buffer FFS  = " << (void *)m_buffer  << "  data = " << (void*) m_data << std::endl;
}

char *BufferFFS::Data() noexcept { return (char *)m_data; }

const char *BufferFFS::Data() const noexcept { return (const char *)m_data; }

void BufferFFS::Delete() { free_FFSBuffer(m_buffer); }
} // end namespace format
} // end namespace adios2
