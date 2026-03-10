// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "inc/Core/SPANN/ExtraFileController.h"
#include "inc/Core/SPANN/IExtraSearcher.h"
#include "inc/Helper/AerospikeKeyValueIO.h"
#include "inc/Test.h"
#include "inc/Helper/TiKVKeyValueIO.h"
#include <chrono>
#include <cstdlib>
#include <memory>

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

using namespace SPTAG;
using namespace SPTAG::SPANN;

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
        db->MultiGet(headIDs, &values, MaxTimeout, &(workspace.m_diskRequests));
        auto t2 = std::chrono::high_resolution_clock::now();
        latency += std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

        if (debug)
        {
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
    	db.reset(new Helper::TiKVKeyValueIO("/tmp/sptag_tikv.sock", 10*1024*1024));
    }
    else if (type == "Aerospike")
    {
        std::string host = SPTAG_AEROSPIKE_DEFAULT_HOST;
        uint16_t port = static_cast<uint16_t>(SPTAG_AEROSPIKE_DEFAULT_PORT);
        std::string ns = SPTAG_AEROSPIKE_DEFAULT_NAMESPACE;
        std::string setName = SPTAG_AEROSPIKE_DEFAULT_SET;
        std::string valueBin = SPTAG_AEROSPIKE_DEFAULT_BIN;

        if (const char* envHost = std::getenv("SPTAG_AEROSPIKE_HOST")) host = envHost;
        if (const char* envPort = std::getenv("SPTAG_AEROSPIKE_PORT")) {
            try { port = static_cast<uint16_t>(std::stoul(envPort)); } catch (...) { port = static_cast<uint16_t>(SPTAG_AEROSPIKE_DEFAULT_PORT); }
        }
        if (const char* envNs = std::getenv("SPTAG_AEROSPIKE_NAMESPACE")) ns = envNs;
        if (const char* envSet = std::getenv("SPTAG_AEROSPIKE_SET")) setName = envSet;
        if (const char* envBin = std::getenv("SPTAG_AEROSPIKE_BIN")) valueBin = envBin;

        db.reset(new Helper::AerospikeKeyValueIO(host, port, ns, setName, valueBin));
    }

    else
    {
        db.reset(new FileIO(opt));
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < totalNum; i++)
    {
        int len = std::to_string(i).length();
        std::string val(PageSize - len, '0');
        
	if (db->Put(i, val, MaxTimeout, &(workspace.m_diskRequests)) != ErrorCode::Success) {
            std::cerr << "TiKV Put operation failed on key: " << i << std::endl;
        }	
    }
    auto t2 = std::chrono::high_resolution_clock::now();
    std::cout << "avg put time: "
              << (std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() / (float)(totalNum)) << "us"
              << std::endl;

    db->ForceCompaction();

    t1 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < totalNum; i++)
    {
        for (int j = 0; j < mergeIters; j++)
        {
            if (db->Merge(i, std::to_string(i), MaxTimeout, &(workspace.m_diskRequests),
                      [](const void* val, const int size) -> bool { return true; }) != ErrorCode::Success) {
                std::cerr << "TiKV Merge operation failed on key: " << i << std::endl;
            }
	}
    }
    t2 = std::chrono::high_resolution_clock::now();
    std::cout << "avg merge time: "
              << (std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count() /
                  (float)(totalNum * mergeIters))
              << "us" << std::endl;

    std::string value;
    if (db->Get(0, &value, MaxTimeout, &(workspace.m_diskRequests)) != ErrorCode::Success || value.empty()) {
        std::cerr << "Get operation failed on key: 0" << std::endl;
    }

    if (db->Delete(0) != ErrorCode::Success) {
        std::cerr << "Delete operation failed on key: 0" << std::endl;
    }

    std::string deletedValue;
    if (db->Get(0, &deletedValue, MaxTimeout, &(workspace.m_diskRequests)) == ErrorCode::Success) {
        std::cerr << "Delete validation failed: key 0 still exists." << std::endl;
    }

    Search(db, internalResultNum, totalNum, 10, debug, workspace);

    db->ForceCompaction();
    db->ShutDown();

    // if (type == "RocksDB") {
    //     db.reset(new RocksDBIO(path.c_str(), true));
    //     Search(db, internalResultNum, totalNum, 10, debug);
    //     db->ForceCompaction();
    //     db->ShutDown();
    // }
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
    Test(std::string("tmp_tikv") + FolderSep + "test", "TiKV", false);
}

BOOST_AUTO_TEST_CASE(AerospikeTest)
{
    Test(std::string("tmp_aerospike") + FolderSep + "test", "Aerospike", false);
}

BOOST_AUTO_TEST_SUITE_END()
