/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * DataMan.cpp  classes for DataMan streaming format
 *
 *  Created on: May 11, 2018
 *      Author: Jason Wang
 */

#include "DataMan.tcc"

#include <cstring> //std::memcpy
#include <iostream>

namespace adios2
{
namespace format
{

void DataManSerializer::New(size_t size)
{
    m_Buffer = std::make_shared<std::vector<char>>();
    m_Buffer->reserve(size);
    m_Position = 0;
}

const std::shared_ptr<std::vector<char>> DataManSerializer::Get()
{
    return m_Buffer;
}

void DataManDeserializer::Put(std::shared_ptr<std::vector<char>> data)
{
    int key = rand();
    m_MutexBuffer.lock();
    while (m_BufferMap.count(key) > 0)
    {
        key = rand();
    }
    m_BufferMap[key] = data;
    m_MutexBuffer.unlock();
    size_t position = 0;
    while (position < data->capacity())
    {
        uint32_t metasize;
        std::memcpy(&metasize, data->data() + position, sizeof(metasize));
        position += sizeof(metasize);
        if (position + metasize > data->size())
        {
            break;
        }
        DataManVar var;
        try
        {
            nlohmann::json metaj =
                nlohmann::json::parse(data->data() + position);
            position += metasize;
            var.name = metaj["N"].get<std::string>();
            var.type = metaj["Y"].get<std::string>();
            var.shape = metaj["S"].get<Dims>();
            var.count = metaj["C"].get<Dims>();
            var.start = metaj["O"].get<Dims>();
            var.step = metaj["T"].get<size_t>();
            var.size = metaj["I"].get<size_t>();
            var.rank = metaj["R"].get<int>();
            var.doid = metaj["D"].get<std::string>();
            var.position = position;
            var.index = key;
            auto it = metaj.find("Z");
            if (it != metaj.end())
            {
                var.compression = it->get<std::string>();
            }
            it = metaj.find("ZR");
            if (it != metaj.end())
            {
                var.compressionRate = it->get<float>();
            }
            if (position + var.size > data->capacity())
            {
                break;
            }
            m_MutexMetaData.lock();
            if (m_MetaDataMap[var.step] == nullptr)
            {
                m_MetaDataMap[var.step] =
                    std::make_shared<std::vector<DataManVar>>();
            }
            m_MetaDataMap[var.step]->push_back(std::move(var));
            m_MutexMetaData.unlock();
            position += var.size;
        }
        catch (std::exception &e)
        {
            std::cout << e.what() << std::endl;
        }
        m_MutexMaxMin.lock();
        if (m_MaxStep < var.step)
        {
            m_MaxStep = var.step;
        }
        if (m_MinStep > var.step)
        {
            m_MinStep = var.step;
        }
        m_MutexMaxMin.unlock();
    }
}

void DataManDeserializer::Erase(size_t step)
{
    m_MutexMetaData.lock();
    const auto &i = m_MetaDataMap.find(step);
    if (i != m_MetaDataMap.end())
    {
        for (const auto &k : *i->second)
        {
            if (BufferContainsSteps(k.index, step + 1, MaxStep()) == false)
            {
                m_MutexBuffer.lock();
                m_BufferMap.erase(k.index);
                m_MutexBuffer.unlock();
            }
        }
    }
    m_MetaDataMap.erase(step);
    m_MutexMetaData.unlock();

    m_MutexMaxMin.lock();
    m_MinStep = step + 1;
    m_MutexMaxMin.unlock();
}

size_t DataManDeserializer::MaxStep()
{
    std::lock_guard<std::mutex> l(m_MutexMaxMin);
    return m_MaxStep;
}

size_t DataManDeserializer::MinStep()
{
    std::lock_guard<std::mutex> l(m_MutexMaxMin);
    return m_MinStep;
}

const std::shared_ptr<std::vector<DataManDeserializer::DataManVar>>
DataManDeserializer::GetMetaData(size_t step)
{
    std::lock_guard<std::mutex> l(m_MutexMetaData);
    const auto &i = m_MetaDataMap.find(step);
    if (i != m_MetaDataMap.end())
    {
        return m_MetaDataMap[step];
    }
    else
    {
        return nullptr;
    }
}

bool DataManDeserializer::BufferContainsSteps(int index, size_t begin,
                                              size_t end)
{
    // This is a private function and is always called after m_MutexMetaData is
    // locked, so there is no need to lock again here.
    for (size_t i = begin; i <= end; ++i)
    {
        const auto &j = m_MetaDataMap.find(i);
        if (j != m_MetaDataMap.end())
        {
            for (const auto &k : *j->second)
            {
                if (k.index == index)
                {
                    return true;
                }
            }
        }
    }
    return false;
}

Dims DataManDeserializer::GetRelativePosition(const Dims &inner,
                                              const Dims &outer)
{
    Dims ret;
    size_t size = inner.size();
    ret.resize(size);
    for (int i = 0; i < size; ++i)
    {
        ret[i] = inner[i] - outer[i];
    }
    return ret;
}

Dims DataManDeserializer::GetAbsolutePosition(const Dims &inner,
                                              const Dims &outer)
{
    Dims ret;
    size_t size = inner.size();
    ret.resize(size);
    for (int i = 0; i < size; ++i)
    {
        ret[i] = inner[i] + outer[i];
    }
    return ret;
}

size_t DataManDeserializer::MultiToOne(const Dims &global, const Dims &position)
{
    size_t index = 0;
    for (int i = 1; i < global.size(); ++i)
    {
        index += std::accumulate(global.begin() + i, global.end(),
                                 position[i - 1], std::multiplies<size_t>());
    }
    index += position.back();
    return index;
}

Dims DataManDeserializer::OneToMulti(const Dims &global, size_t position)
{
    std::vector<size_t> index(global.size());
    for (int i = 1; i < global.size(); ++i)
    {
        size_t s = std::accumulate(global.begin() + i, global.end(), 1,
                                   std::multiplies<size_t>());
        index[i - 1] = position / s;
        position -= index[i - 1] * s;
    }
    index.back() = position;
    return index;
}

void DataManDeserializer::CopyLocalToGlobal(char *dst, const Box<Dims> &dstBox,
                                            const char *src,
                                            const Box<Dims> &srcBox,
                                            const size_t size,
                                            const Box<Dims> &overlapBox)
{

    size_t dimensions = overlapBox.first.size();
    size_t overlapSize = 1;
    for (int i = 0; i < dimensions; ++i)
    {
        overlapSize =
            overlapSize * (overlapBox.second[i] - overlapBox.first[i]);
    }

    Dims srcCount(dimensions);
    for (int i = 0; i < dimensions; ++i)
    {
        srcCount[i] = srcBox.second[i] - srcBox.first[i];
    }

    Dims dstCount(dimensions);
    for (int i = 0; i < dimensions; ++i)
    {
        dstCount[i] = dstBox.second[i] - dstBox.first[i];
    }

    if (IsContinuous(overlapBox, srcBox) && IsContinuous(overlapBox, dstBox))
    {
        Dims overlapInSrcRelativeLeftBoundary =
            GetRelativePosition(overlapBox.first, srcBox.first);
        Dims overlapInDstRelativeLeftBoundary =
            GetRelativePosition(overlapBox.first, dstBox.first);
        size_t srcStartPtrOffset =
            MultiToOne(srcCount, overlapInSrcRelativeLeftBoundary);
        size_t dstStartPtrOffset =
            MultiToOne(dstCount, overlapInDstRelativeLeftBoundary);
        std::memcpy(dst + dstStartPtrOffset * size,
                    src + srcStartPtrOffset * size, overlapSize * size);
    }
    else
    {
        size_t overlapChunkSize =
            (overlapBox.second.back() - overlapBox.first.back());

        Dims overlapCount(dimensions);
        for (int i = 0; i < dimensions; ++i)
        {
            overlapCount[i] = overlapBox.second[i] - overlapBox.first[i];
        }

        for (size_t i = 0; i < overlapSize; i += overlapChunkSize)
        {
            Dims currentPositionLocal = OneToMulti(overlapCount, i);
            Dims currentPositionGlobal =
                GetAbsolutePosition(currentPositionLocal, overlapBox.first);
            Dims overlapInSrcRelativeCurrentPosition =
                GetRelativePosition(currentPositionGlobal, srcBox.first);
            Dims overlapInDstRelativeCurrentPosition =
                GetRelativePosition(currentPositionGlobal, dstBox.first);
            size_t srcStartPtrOffset =
                MultiToOne(srcCount, overlapInSrcRelativeCurrentPosition);
            size_t dstStartPtrOffset =
                MultiToOne(dstCount, overlapInDstRelativeCurrentPosition);
            std::memcpy(dst + dstStartPtrOffset * size,
                        src + srcStartPtrOffset * size,
                        overlapChunkSize * size);
        }
    }
}

bool DataManDeserializer::GetOverlap(const Box<Dims> &b1, const Box<Dims> &b2,
                                     Box<Dims> &overlapBox)
{
    overlapBox.first.resize(b1.first.size());
    overlapBox.second.resize(b1.first.size());

    for (size_t i = 0; i < b1.first.size(); ++i)
    {
        if (b1.first[i] > b2.first[i])
        {
            overlapBox.first[i] = b1.first[i];
        }
        else
        {
            overlapBox.first[i] = b2.first[i];
        }
        if (b1.second[i] < b2.second[i])
        {
            overlapBox.second[i] = b1.second[i];
        }
        else
        {
            overlapBox.second[i] = b2.second[i];
        }
    }

    for (size_t i = 0; i < overlapBox.first.size(); ++i)
    {
        if (overlapBox.first[i] > overlapBox.second[i])
        {
            return false;
        }
    }

    return true;
}

bool DataManDeserializer::IsContinuous(const Box<Dims> &inner,
                                       const Box<Dims> &outer)
{
    for (size_t i = 1; i < inner.first.size(); ++i)
    {
        if (inner.first[i] != outer.first[i])
        {
            return false;
        }
        if (inner.second[i] != outer.second[i])
        {
            return false;
        }
    }
    return true;
}

bool DataManDeserializer::GetVarList(size_t step,
                                     std::vector<DataManVar> &varList)
{
    m_MutexMetaData.lock();
    auto metaDataStep = m_MetaDataMap.find(step);
    if (metaDataStep == m_MetaDataMap.end())
    {
        return false;
    }
    for (auto &i : *metaDataStep->second)
    {
        bool hasVar = false;
        for (DataManVar &j : varList)
        {
            if (j.name == i.name)
            {
                hasVar = true;
            }
        }
        if (hasVar == false)
        {
            DataManVar var;
            var.name = i.name;
            var.shape = i.shape;
            var.type = i.type;
            var.doid = i.doid;
            varList.push_back(std::move(var));
        }
    }
    m_MutexMetaData.unlock();
    return true;
}

void DataManDeserializer::PrintBox(const Box<Dims> in, std::string name)
{

    std::cout << name << " Left boundary: [";
    for (auto &i : in.first)
    {
        std::cout << i << ", ";
    }
    std::cout << "]" << std::endl;

    std::cout << name << " Right boundary: [";
    for (auto &i : in.second)
    {
        std::cout << i << ", ";
    }
    std::cout << "]" << std::endl;
}

} // namespace format
} // namespace adios2
