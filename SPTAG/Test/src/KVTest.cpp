// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/SPANN/ExtraFileController.h"
#include "inc/Core/SPANN/IExtraSearcher.h"
#include "inc/Helper/AerospikeKeyValueIO.h"
#include "inc/Test.h"
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>

#ifndef SPTAG_AEROSPIKE_DEFAULT_HOST
#define SPTAG_AEROSPIKE_DEFAULT_HOST "10.150.0.24"
#endif

#ifndef SPTAG_AEROSPIKE_DEFAULT_PORT
#define SPTAG_AEROSPIKE_DEFAULT_PORT 3000
#endif

#ifndef SPTAG_AEROSPIKE_DEFAULT_NAMESPACE
#define SPTAG_AEROSPIKE_DEFAULT_NAMESPACE "test"
#endif

#ifndef SPTAG_AEROSPIKE_DEFAULT_SET
#define SPTAG_AEROSPIKE_DEFAULT_SET "sptag"
#endif

#ifndef SPTAG_AEROSPIKE_DEFAULT_BIN
#define SPTAG_AEROSPIKE_DEFAULT_BIN "value"
#endif

// enable rocksdb io_uring

#ifdef ROCKSDB
#include "inc/Core/SPANN/ExtraRocksDBController.h"
// extern "C" bool RocksDbIOUringEnable() { return true; }
#endif

#ifdef SPDK
#include "inc/Core/SPANN/ExtraSPDKController.h"
#endif

#ifdef TIKV
#include "inc/Core/SPANN/ExtraTiKVController.h"
#endif

using namespace SPTAG;
using namespace SPTAG::SPANN;

namespace
{
bool IsTruthyEnvValue(const char* value)
{
    if (value == nullptr)
    {
        return false;
    }

    std::string normalized(value);
    for (char& ch : normalized)
    {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return normalized == "1" || normalized == "true" || normalized == "yes" || normalized == "on";
}

uint16_t ParseAerospikePort(const char* envPort, uint16_t fallback, bool* usedFallback)
{
    if (usedFallback != nullptr)
    {
        *usedFallback = false;
    }

    if (envPort == nullptr)
    {
        return fallback;
    }

    try
    {
        auto parsed = std::stoul(envPort);
        if (parsed > 0 && parsed <= std::numeric_limits<uint16_t>::max())
        {
            return static_cast<uint16_t>(parsed);
        }
    }
    catch (...)
    {
    }

    if (usedFallback != nullptr)
    {
        *usedFallback = true;
    }
    return fallback;
}
} // namespace

void Search(std::shared_ptr<Helper::KeyValueIO> db, int internalResultNum, int totalSize, int times, bool debug,
            SPTAG::SPANN::ExtraWorkSpace &workspace)
{
    std::vector<SizeType> headIDs(internalResultNum, 0);

    std::vector<std::string> values;
    double latency = 0;
    for (int i = 0; i < times; i++)
    {
        values.clear();
        for (int j = 0; j < internalResultNum; j++)
            headIDs[j] = (j + i * internalResultNum) % totalSize;
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ret = db->MultiGet(headIDs, &values, MaxTimeout, &(workspace.m_diskRequests));
        auto t2 = std::chrono::high_resolution_clock::now();
        latency += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        BOOST_REQUIRE_MESSAGE(ret == ErrorCode::Success, "Search MultiGet failed");

        if (debug)
        {
            BOOST_REQUIRE_MESSAGE(values.size() >= static_cast<size_t>(internalResultNum), "Search returned insufficient values");
            for (int j = 0; j < internalResultNum; j++)
            {
                std::cout << values[j].substr(PageSize) << std::endl;
            }
        }
    }
    std::cout << "avg get time: " << (latency / (float)(times)) << "us" << std::endl;
}

void Test(std::string path, std::string type, bool debug = false)
{
    int internalResultNum = 64;
    int totalNum = 1024;
    int mergeIters = 3;
    std::shared_ptr<Helper::KeyValueIO> db;
    SPTAG::SPANN::ExtraWorkSpace workspace;
    SPTAG::SPANN::Options opt;
    auto idx = path.find_last_of(FolderSep);
    opt.m_indexDirectory = path.substr(0, idx);
    opt.m_ssdMappingFile = path.substr(idx + 1);
    workspace.Initialize(4096, 2, internalResultNum, 4 * PageSize, true, false);

    if (type == "RocksDB")
    {
#ifdef ROCKSDB
        db.reset(new RocksDBIO(path.c_str(), true));
#else
        {
            std::cerr << "RocksDB is not supported in this build." << std::endl;
            return;
        }
#endif
    }
    else if (type == "SPDK")
    {
#ifdef SPDK
        db.reset(new SPDKIO(opt));
#else
        {
            std::cerr << "SPDK is not supported in this build." << std::endl;
            return;
        }
#endif
    }

    else if (type == "TiKV")
    {
#ifdef TIKV
        std::string pdAddresses = "127.0.0.1:2379";
        std::string keyPrefix = "kvtest";
        if (const char *envPd = std::getenv("SPTAG_TIKV_PD"))
        {
            pdAddresses = envPd;
        }
        if (const char *envPrefix = std::getenv("SPTAG_TIKV_PREFIX"))
        {
            keyPrefix = envPrefix;
        }
        db.reset(new TiKVIO(pdAddresses, keyPrefix));
#else
        std::cerr << "TiKV is not supported in this build. Rebuild with -DTIKV=ON." << std::endl;
        return;
#endif
    }
    else if (type == "Aerospike")
    {
        std::string host = SPTAG_AEROSPIKE_DEFAULT_HOST;
        uint16_t port = static_cast<uint16_t>(SPTAG_AEROSPIKE_DEFAULT_PORT);
        std::string ns = SPTAG_AEROSPIKE_DEFAULT_NAMESPACE;
        std::string setName = SPTAG_AEROSPIKE_DEFAULT_SET;
        std::string valueBin = SPTAG_AEROSPIKE_DEFAULT_BIN;
        std::string user;
        std::string password;

        if (const char* envHost = std::getenv("SPTAG_AEROSPIKE_HOST")) host = envHost;
        if (const char* envPort = std::getenv("SPTAG_AEROSPIKE_PORT")) {
            bool usedFallback = false;
            port = ParseAerospikePort(envPort, static_cast<uint16_t>(SPTAG_AEROSPIKE_DEFAULT_PORT), &usedFallback);
            if (usedFallback)
            {
                std::cerr << "Invalid SPTAG_AEROSPIKE_PORT=\"" << envPort
                          << "\". Falling back to default port " << SPTAG_AEROSPIKE_DEFAULT_PORT << std::endl;
            }
        }
        if (const char* envNs = std::getenv("SPTAG_AEROSPIKE_NAMESPACE")) ns = envNs;
        if (const char* envSet = std::getenv("SPTAG_AEROSPIKE_SET")) setName = envSet;
        if (const char* envBin = std::getenv("SPTAG_AEROSPIKE_BIN")) valueBin = envBin;
        if (const char* envUser = std::getenv("SPTAG_AEROSPIKE_USER")) user = envUser;
        if (const char* envPassword = std::getenv("SPTAG_AEROSPIKE_PASSWORD")) password = envPassword;

        std::cout << "Aerospike config: host=" << host << " port=" << port << " namespace=" << ns
                  << " set=" << setName << " bin=" << valueBin
                  << " auth=" << (user.empty() ? "disabled" : "enabled") << std::endl;
        db.reset(new Helper::AerospikeKeyValueIO(host, port, ns, setName, valueBin, user, password));
    }

    else
    {
        db.reset(new FileIO(opt));
    }

    BOOST_REQUIRE_MESSAGE(db->Available(), type + " backend is not available (connection failed)");

    if (type == "Aerospike")
    {
        SizeType preflightKey = static_cast<SizeType>(totalNum + 4096);
        std::string preflightValue = "aerospike_preflight_value";
        auto preflightPut = db->Put(preflightKey, preflightValue, MaxTimeout, &(workspace.m_diskRequests));
        BOOST_REQUIRE_MESSAGE(preflightPut == ErrorCode::Success, type + " preflight Put failed");

        std::string preflightRead;
        auto preflightGet = db->Get(preflightKey, &preflightRead, MaxTimeout, &(workspace.m_diskRequests));
        BOOST_REQUIRE_MESSAGE(preflightGet == ErrorCode::Success && preflightRead == preflightValue,
                              type + " preflight Get failed");

        auto preflightDelete = db->Delete(preflightKey);
        BOOST_REQUIRE_MESSAGE(preflightDelete == ErrorCode::Success, type + " preflight Delete failed");
    }

    int putFailures = 0;
    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < totalNum; i++)
    {
        int len = std::to_string(i).length();
        std::string val(PageSize - len, '0');

        if (db->Put(i, val, MaxTimeout, &(workspace.m_diskRequests)) != ErrorCode::Success) {
            std::cerr << type << " Put failed on key: " << i << std::endl;
            ++putFailures;
        }
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    std::cout << "avg put time: "
              << (std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / (float)(totalNum)) << "us"
              << std::endl;
    BOOST_CHECK_EQUAL(putFailures, 0);

    db->ForceCompaction();

    int mergeFailures = 0;
    t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < totalNum; i++)
    {
        for (int j = 0; j < mergeIters; j++)
        {
            if (db->Merge(i, std::to_string(i), MaxTimeout, &(workspace.m_diskRequests),
                      [](const void* val, const int size) -> bool { return true; }) != ErrorCode::Success) {
                std::cerr << type << " Merge failed on key: " << i << std::endl;
                ++mergeFailures;
            }
        }
    }
    t2 = std::chrono::high_resolution_clock::now();
    std::cout << "avg merge time: "
              << (std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() /
                  (float)(totalNum * mergeIters))
              << "us" << std::endl;
    BOOST_CHECK_EQUAL(mergeFailures, 0);

    std::string value;
    auto getResult = db->Get(0, &value, MaxTimeout, &(workspace.m_diskRequests));
    BOOST_CHECK_MESSAGE(getResult == ErrorCode::Success && !value.empty(),
                        type + " Get failed on key 0");

    Search(db, internalResultNum, totalNum, 10, debug, workspace);

    auto deleteResult = db->Delete(0);
    BOOST_CHECK_MESSAGE(deleteResult == ErrorCode::Success,
                        type + " Delete failed on key 0");

    std::string deletedValue;
    auto deletedGetResult = db->Get(0, &deletedValue, MaxTimeout, &(workspace.m_diskRequests));
    BOOST_CHECK_MESSAGE(deletedGetResult != ErrorCode::Success,
                        type + " Delete validation failed: key 0 still exists");

    db->ForceCompaction();
    db->ShutDown();
}

BOOST_AUTO_TEST_SUITE(KVTest)

BOOST_AUTO_TEST_CASE(RocksDBTest)
{
    if (!direxists("tmp_rocksdb"))
        mkdir("tmp_rocksdb");
    Test(std::string("tmp_rocksdb") + FolderSep + "test", "RocksDB", false);
}

BOOST_AUTO_TEST_CASE(SPDKTest)
{
    if (!direxists("tmp_spdk"))
        mkdir("tmp_spdk");
    Test(std::string("tmp_spdk") + FolderSep + "test", "SPDK", false);
}

BOOST_AUTO_TEST_CASE(FileTest)
{
    if (!direxists("tmp_file"))
        mkdir("tmp_file");
    Test(std::string("tmp_file") + FolderSep + "test", "File", false);
}


BOOST_AUTO_TEST_CASE(TiKVTest)
{
#ifndef TIKV
    BOOST_TEST_MESSAGE("Skipping KVTest/TiKVTest: SPTAG was built without TiKV support (-DTIKV=ON).");
    return;
#else
    Test(std::string("tmp_tikv") + FolderSep + "test", "TiKV", false);
#endif
}

BOOST_AUTO_TEST_CASE(AerospikeTest)
{
#ifndef AEROSPIKE
    BOOST_TEST_MESSAGE("Skipping KVTest/AerospikeTest: SPTAG was built without Aerospike support (-DAEROSPIKE=ON).");
    return;
#else
    if (!IsTruthyEnvValue(std::getenv("SPTAG_RUN_AEROSPIKE_TEST")))
    {
        BOOST_TEST_MESSAGE("Skipping KVTest/AerospikeTest: set SPTAG_RUN_AEROSPIKE_TEST=1 to opt in.");
        return;
    }
    Test(std::string("tmp_aerospike") + FolderSep + "test", "Aerospike", false);
#endif
}

BOOST_AUTO_TEST_SUITE_END()
