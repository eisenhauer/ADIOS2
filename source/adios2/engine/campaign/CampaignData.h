/*
 * Distributed under the OSI-approved Apache License, Version 2.0.  See
 * accompanying file Copyright.txt for details.
 *
 * CampaignData.h
 * Campaign data from database
 *
 *  Created on: May 16, 2023
 *      Author: Norbert Podhorszki pnorbert@ornl.gov
 */

#ifndef ADIOS2_ENGINE_CAMPAIGNDATA_H_
#define ADIOS2_ENGINE_CAMPAIGNDATA_H_

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <sqlite3.h>

namespace adios2
{
namespace core
{
namespace engine
{

struct CampaignHost
{
    std::string hostname;
    std::string longhostname;
    std::vector<size_t> dirIdx; // index in CampaignData.directory global list of dirs
};

struct CampaignKey
{
    std::string id;
    std::string keyHex; // sodium key in hex format
};

struct CampaignBPFile
{
    std::string name;
    size_t bpDatasetIdx; // index of parent CampaignBPDataset in the map
    bool compressed;
    size_t lengthOriginal;
    size_t lengthCompressed;
    int64_t ctime;
};

struct CampaignBPDataset
{
    std::string uuid;
    std::string name;
    size_t hostIdx;
    size_t dirIdx;
    bool hasKey;
    size_t keyIdx;
    std::vector<CampaignBPFile> files;
};

struct CampaignVersion
{
    std::string versionStr;
    int major;
    int minor;
    int micro;
    double version;
};

struct CampaignData
{
    CampaignVersion version;
    std::vector<CampaignHost> hosts;
    std::vector<CampaignKey> keys;
    std::vector<std::string> directory;
    std::map<size_t, CampaignBPDataset> bpdatasets;
};

void ReadCampaignData(sqlite3 *db, CampaignData &cd);

void SaveToFile(sqlite3 *db, const std::string &path, const CampaignBPFile &bpfile,
                std::string &keyHex);

} // end namespace engine
} // end namespace core
} // end namespace adios2

#endif /* ADIOS2_ENGINE_CAMPAIGDATA_H_ */
