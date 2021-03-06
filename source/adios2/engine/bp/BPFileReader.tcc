/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * BPFileReader.tcc
 *
 *  Created on: Feb 27, 2017
 *      Author: William F Godoy godoywf@ornl.gov
 */

#ifndef ADIOS2_ENGINE_BP_BPFILEREADER_TCC_
#define ADIOS2_ENGINE_BP_BPFILEREADER_TCC_

#include "BPFileReader.h"

namespace adios2
{
namespace core
{
namespace engine
{

template <>
inline void BPFileReader::GetSyncCommon(Variable<std::string> &variable,
                                        std::string *data)
{
    variable.m_Data = data;
    m_BP3Deserializer.GetValueFromMetadata(variable);
}

template <class T>
inline void BPFileReader::GetSyncCommon(Variable<T> &variable, T *data)
{
    variable.m_Data = data;

    if (variable.m_SingleValue)
    {
        m_BP3Deserializer.GetValueFromMetadata(variable);
        return;
    }

    const std::map<std::string, helper::SubFileInfoMap> variableSubfileInfo =
        m_BP3Deserializer.GetSyncVariableSubFileInfo(variable);

    ReadVariables(variableSubfileInfo);
}

template <class T>
void BPFileReader::GetDeferredCommon(Variable<T> &variable, T *data)
{
    // returns immediately without populating data
    m_BP3Deserializer.GetDeferredVariable(variable, data);
    m_BP3Deserializer.m_PerformedGets = false;
}

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif /* ADIOS2_ENGINE_BP_BPFILEREADER_TCC_ */
