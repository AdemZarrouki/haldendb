#include <iostream>
#include "LRUCache.hpp"
#include "BPlusStore.hpp"
#include "NoCache.hpp"
#include "glog/logging.h"
#include <type_traits>
#include <variant>
#include <typeinfo>
#include "DataNode.hpp"
#include "IndexNode.hpp"
#include "DataNodeROpt.hpp"
#include "IndexNodeROpt.hpp"
#include <chrono>
#include <cassert>
#include "VolatileStorage.hpp"
#include "NoCacheObject.hpp"
#include "LRUCacheObject.hpp"
#include "FileStorage.hpp"
#include "TypeMarshaller.hpp"
#include "PMemStorage.hpp"
#include "TypeUID.h"
#include <iostream>
#include "ObjectFatUID.h"
#include "ObjectUID.h"
#include <set>
#include <random>
#include <numeric>

#define __VALIDITY_CHECK__

#ifdef _MSC_VER
#define FILE_STORAGE_PATH "c:\\filestore.hdb"
#define PMEM_STORAGE_PATH "c:\\filestore.hdb"
#define PMEM_STORAGE_PATH_II "/mnt/tmpfs/datafile2"
#else
#define FILE_STORAGE_PATH "/mnt/tmpfs/filestore.hdb"
#define PMEM_STORAGE_PATH "/mnt/tmpfs/datafile1"
#define PMEM_STORAGE_PATH_II "/mnt/tmpfs/datafile2"
#endif

#ifdef __CONCURRENT__
template <typename BPlusStoreType>
void insert_concurent(BPlusStoreType* ptrTree, int nRangeStart, int nRangeEnd)
{
    std::vector<int> random_numbers(nRangeEnd - nRangeStart);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), nRangeStart); // Fill vector with 1 to 5,000,000
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    for (size_t nCntr = 0; nCntr < nRangeEnd - nRangeStart; nCntr++)
    {
        ErrorCode ec = ptrTree->insert(random_numbers[nCntr], random_numbers[nCntr]);
        assert(ec == ErrorCode::Success);
    }
}

template <typename BPlusStoreType>
void reverse_insert_concurent(BPlusStoreType* ptrTree, int nRangeStart, int nRangeEnd)
{
    for (int nCntr = nRangeEnd - 1; nCntr >= nRangeStart; nCntr--)
    {
        ErrorCode ec = ptrTree->insert(nCntr, nCntr);
        assert(ec == ErrorCode::Success);
    }
}

template <typename BPlusStoreType>
void search_concurent(BPlusStoreType* ptrTree, int nRangeStart, int nRangeEnd)
{
    std::vector<int> random_numbers(nRangeEnd - nRangeStart);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), nRangeStart); // Fill vector with 1 to 5,000,000
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    for (size_t nCntr = 0; nCntr < nRangeEnd - nRangeStart; nCntr++)
    {
        int nValue = 0;
        ErrorCode ec = ptrTree->search(random_numbers[nCntr], nValue);

        assert(random_numbers[nCntr] == nValue && ec == ErrorCode::Success);
    }
}

template <typename BPlusStoreType>
void search_not_found_concurent(BPlusStoreType* ptrTree, int nRangeStart, int nRangeEnd) {
    for (size_t nCntr = nRangeStart; nCntr < nRangeEnd; nCntr++)
    {
        int nValue = 0;
        ErrorCode ec = ptrTree->search(nCntr, nValue);

        assert(ec == ErrorCode::KeyDoesNotExist);
    }
}

template <typename BPlusStoreType>
void delete_concurent(BPlusStoreType* ptrTree, int nRangeStart, int nRangeEnd) {
    std::vector<int> random_numbers(nRangeEnd - nRangeStart);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), nRangeStart); // Fill vector with 1 to 5,000,000
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    for (size_t nCntr = 0; nCntr < nRangeEnd - nRangeStart; nCntr++)
    {
        ErrorCode ec = ptrTree->remove(random_numbers[nCntr]);

        assert(ec == ErrorCode::Success);
    }
}

template <typename BPlusStoreType>
void reverse_delete_concurent(BPlusStoreType* ptrTree, int nRangeStart, int nRangeEnd) {
    for (int nCntr = nRangeEnd - 1; nCntr >= nRangeStart; nCntr--)
    {
        ErrorCode ec = ptrTree->remove(nCntr);

        assert(ec == ErrorCode::KeyDoesNotExist);
    }
}

template <typename BPlusStoreType>
void threaded_test(BPlusStoreType* ptrTree, int degree, int total_entries, int thread_count)
{
    vector<std::thread> vtThreads;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (size_t nTestCntr = 0; nTestCntr < 1; nTestCntr++) {

        for (int nIdx = 0; nIdx < thread_count; nIdx++)
        {
            int nTotal = total_entries / thread_count;
            vtThreads.push_back(std::thread(insert_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
        }

        auto it = vtThreads.begin();
        while (it != vtThreads.end())
        {
            (*it).join();
            it++;
        }

        vtThreads.clear();

        for (int nIdx = 0; nIdx < thread_count; nIdx++)
        {
            int nTotal = total_entries / thread_count;
            vtThreads.push_back(std::thread(search_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
        }

        it = vtThreads.begin();
        while (it != vtThreads.end())
        {
            (*it).join();
            it++;
        }

        vtThreads.clear();

        for (int nIdx = 0; nIdx < thread_count; nIdx++)
        {
            int nTotal = total_entries / thread_count;
            vtThreads.push_back(std::thread(delete_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
        }   

        it = vtThreads.begin();
        while (it != vtThreads.end())
        {
            (*it).join();
            it++;
        }

        vtThreads.clear();

        for (int nIdx = 0; nIdx < thread_count; nIdx++)
        {
            int nTotal = total_entries / thread_count;
            vtThreads.push_back(std::thread(search_not_found_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
        }

        it = vtThreads.begin();
        while (it != vtThreads.end())
        {
            (*it).join();
            it++;
        }

        vtThreads.clear();

#ifdef __TREE_WITH_CACHE__
        size_t nLRU, nMap;
        ptrTree->getCacheState(nLRU, nMap);

        assert(nLRU == 1 && nMap == 1);
#endif //__TREE_WITH_CACHE__
    }
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> int_test [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
}


template <typename BPlusStoreType>
void fptree_threaded_test(BPlusStoreType* ptrTree, int total_entries, int thread_count)
{
    vector<std::thread> vtThreads;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (int nIdx = 0; nIdx < thread_count; nIdx++)
    {
        int nTotal = total_entries / thread_count;
        vtThreads.push_back(std::thread(insert_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
    }

    auto it = vtThreads.begin();
    while (it != vtThreads.end())
    {
        (*it).join();
        it++;
    }

    vtThreads.clear();

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> insert (threaded) [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(10));

    ptrTree->flush();

    begin = std::chrono::steady_clock::now();

    for (int nIdx = 0; nIdx < thread_count; nIdx++)
    {
        int nTotal = total_entries / thread_count;
        vtThreads.push_back(std::thread(search_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
    }

    it = vtThreads.begin();
    while (it != vtThreads.end())
    {
        (*it).join();
        it++;
    }

    vtThreads.clear();

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> search (threaded) [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(10));

    begin = std::chrono::steady_clock::now();

    for (int nIdx = 0; nIdx < thread_count; nIdx++)
    {
        int nTotal = total_entries / thread_count;
        vtThreads.push_back(std::thread(delete_concurent<BPlusStoreType>, ptrTree, nIdx * nTotal, nIdx * nTotal + nTotal));
    }

    it = vtThreads.begin();
    while (it != vtThreads.end())
    {
        (*it).join();
        it++;
    }

    vtThreads.clear();

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> delete (threaded) [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

#ifdef __TREE_WITH_CACHE__
        size_t nLRU, nMap;
        ptrTree->getCacheState(nLRU, nMap);

        assert(nLRU == 1 && nMap == 1);
#endif //__TREE_WITH_CACHE__
}
#endif //__CONCURRENT__

template <typename BPlusStoreType>
void int_test(BPlusStoreType* ptrTree, size_t nMaxNumber)
{
    std::vector<int> random_numbers(nMaxNumber);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), 1); // Fill vector with 1 to 5,000,000

    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (size_t nTestCntr = 0; nTestCntr < 2; nTestCntr++)
    {
        for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr = nCntr + 1)
        {
            ErrorCode code = ptrTree->insert(random_numbers[nCntr], random_numbers[nCntr]);
            assert(code == ErrorCode::Success);
        }

        //std::ofstream out_1("d:\\tree_post_insert_0.txt");
        //ptrTree->print(out_1);
        //out_1.flush();
        //out_1.close();

        for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode code = ptrTree->search(random_numbers[nCntr], nValue);

            assert(nValue == random_numbers[nCntr]);
        }

        for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr = nCntr + 2)
        {
            ErrorCode code = ptrTree->remove(random_numbers[nCntr]);

            assert(code == ErrorCode::Success);
        }
        for (size_t nCntr = 1; nCntr < nMaxNumber; nCntr = nCntr + 2)
        {
            ErrorCode code = ptrTree->remove(random_numbers[nCntr]);

            assert(code == ErrorCode::Success);
        }

        for (int nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode code = ptrTree->search(random_numbers[nCntr], nValue);

            assert(code == ErrorCode::KeyDoesNotExist);
        }

#ifdef __TREE_WITH_CACHE__
        size_t nLRU, nMap;
        ptrTree->getCacheState(nLRU, nMap);

        assert(nLRU == 1 && nMap == 1);
#endif //__TREE_WITH_CACHE__
}

    for (size_t nTestCntr = 0; nTestCntr < 2; nTestCntr++)
    {
        for (int nCntr = nMaxNumber; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = ptrTree->insert(nCntr, nCntr);
            assert(ec == ErrorCode::Success);

        }
        for (int nCntr = nMaxNumber - 1; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = ptrTree->insert(nCntr, nCntr);
            assert(ec == ErrorCode::Success);
        }

        for (int nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode ec = ptrTree->search(nCntr, nValue);

            assert(nValue == nCntr && ec == ErrorCode::Success);
        }

        for (int nCntr = nMaxNumber; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = ptrTree->remove(nCntr);
            assert(ec == ErrorCode::Success);
        }

        for (int nCntr = nMaxNumber - 1; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = ptrTree->remove(nCntr);
            assert(ec == ErrorCode::Success);
        }

        for (int nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode ec = ptrTree->search(nCntr, nValue);

            assert(ec == ErrorCode::KeyDoesNotExist);
    }

#ifdef __TREE_WITH_CACHE__
        size_t nLRU, nMap;
        ptrTree->getCacheState(nLRU, nMap);

        assert(nLRU == 1 && nMap == 1);
#endif //__TREE_WITH_CACHE__
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> int_test [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
}
template <typename BPlusStoreType>
void string_test(BPlusStoreType* ptrTree, int degree, int total_entries)
{
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (size_t nTestCntr = 0; nTestCntr < 10; nTestCntr++)
    {
        for (size_t nCntr = 0; nCntr < total_entries; nCntr = nCntr + 2)
        {
            ptrTree->insert(to_string(nCntr), to_string(nCntr));
        }
        for (size_t nCntr = 1; nCntr < total_entries; nCntr = nCntr + 2)
        {
            ptrTree->insert(to_string(nCntr), to_string(nCntr));
        }

        for (size_t nCntr = 0; nCntr < total_entries; nCntr++)
        {
            string nValue = "";
            ErrorCode code = ptrTree->search(to_string(nCntr), nValue);

            assert(nValue == to_string(nCntr));
        }

        for (size_t nCntr = 0; nCntr < total_entries; nCntr = nCntr + 2)
        {
            ErrorCode code = ptrTree->remove(to_string(nCntr));
        }
        for (size_t nCntr = 1; nCntr < total_entries; nCntr = nCntr + 2)
        {
            ErrorCode code = ptrTree->remove(to_string(nCntr));
        }

        for (size_t nCntr = 0; nCntr < total_entries; nCntr++)
        {
            string nValue = "";
            ErrorCode code = ptrTree->search(to_string(nCntr), nValue);

            assert(code == ErrorCode::KeyDoesNotExist);
        }

#ifdef __TREE_WITH_CACHE__
        size_t nLRU, nMap;
        ptrTree->getCacheState(nLRU, nMap);

        assert(nLRU == 1 && nMap == 1);
#endif //__TREE_WITH_CACHE__
    }

    for (size_t nTestCntr = 0; nTestCntr < 10; nTestCntr++)
    {
        for (int nCntr = total_entries - 1; nCntr >= 0; nCntr = nCntr - 2)
        {
            ptrTree->insert(to_string(nCntr), to_string(nCntr));
        }
        for (int nCntr = total_entries; nCntr >= 0; nCntr = nCntr - 2)
        {
            ptrTree->insert(to_string(nCntr), to_string(nCntr));
        }

        for (int nCntr = 0; nCntr < total_entries; nCntr++)
        {
            string nValue = "";
            ErrorCode code = ptrTree->search(to_string(nCntr), nValue);

            assert(nValue == to_string(nCntr));
        }

        for (int nCntr = total_entries; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode code = ptrTree->remove(to_string(nCntr));
        }
        for (int nCntr = total_entries - 1; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode code = ptrTree->remove(to_string(nCntr));
        }

        for (int nCntr = 0; nCntr < total_entries; nCntr++)
        {
            string nValue = "";
            ErrorCode code = ptrTree->search(to_string(nCntr), nValue);

            assert(code == ErrorCode::KeyDoesNotExist);
        }

#ifdef __TREE_WITH_CACHE__
        size_t nLRU, nMap;
        ptrTree->getCacheState(nLRU, nMap);

        assert(nLRU == 1 && nMap == 1);
#endif //__TREE_WITH_CACHE__
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> int_test [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
}

void test_for_ints()
{
    for( size_t nDegree = 1000; nDegree < 2000; nDegree = nDegree + 200)
    {
        std::cout << "||||||| Running 'test_for_ints' for nDegree:" << nDegree << std::endl;
        
#ifndef __TREE_WITH_CACHE__
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef uintptr_t ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef BPlusStore<KeyType, ValueType, NoCache<ObjectUIDType, NoCacheObject, DataNodeType, IndexNodeType>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree);
            ptrTree.template init<DataNodeType>();

            int_test<BPlusStoreType>(&ptrTree, 5000000);
        }
#else //__TREE_WITH_CACHE__
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, VolatileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024);
            ptrTree.template init<DataNodeType>();

            int_test<BPlusStoreType>(&ptrTree, 1000000);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, FileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024* 1024 * 1024, FILE_STORAGE_PATH);
            ptrTree.init<DataNodeType>();

            int_test<BPlusStoreType>(&ptrTree, 1000000);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, PMemStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
#ifndef _MSC_VER
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, PMEM_STORAGE_PATH);
            ptrTree.init<DataNodeType>();
            int_test<BPlusStoreType>(&ptrTree, 1000000);
#endif //_MSC_VER
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, VolatileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024);
            ptrTree.template init<DataNodeType>();

            int_test<BPlusStoreType>(&ptrTree, 1000000);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, FileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);
            ptrTree.init<DataNodeType>();

            int_test<BPlusStoreType>(&ptrTree, 1000000);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, PMemStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
#ifndef _MSC_VER
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, PMEM_STORAGE_PATH);
            ptrTree.init<DataNodeType>();
            int_test<BPlusStoreType>(&ptrTree, 1000000);
#endif //_MSC_VER
        }
#endif //__TREE_WITH_CACHE__

        std::cout << std::endl;
    }
}

void test_for_string()
{
    for (size_t nDegree = 3; nDegree < 40; nDegree++) 
    {
        std::cout << "||||||| Running 'test_for_string' for nDegree:" << nDegree << std::endl;

#ifndef __TREE_WITH_CACHE__
        {
            typedef string KeyType;
            typedef string ValueType;
            typedef uintptr_t ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_STRING_STRING> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_STRING_STRING> IndexNodeType;

            typedef BPlusStore<KeyType, ValueType, NoCache<ObjectUIDType, NoCacheObject, DataNodeType, IndexNodeType>> BPlusStoreType;
            BPlusStoreType* ptrTree1 = new BPlusStoreType(nDegree);
            ptrTree1->init<DataNodeType>();

            string_test<BPlusStoreType>(ptrTree1, nDegree, 100000);
        }
#else //__TREE_WITH_CACHE__
        {
        }
#endif //__TREE_WITH_CACHE__
        std::cout << std::endl;
    }
}

void test_for_threaded()
{
#ifdef __CONCURRENT__
    for (size_t nDegree = 200; nDegree < 1200; nDegree = nDegree + 200)
    {
        std::cout << "||||||| Running 'test_for_threaded' for nDegree:" << nDegree << std::endl;
//continue;
#ifndef __TREE_WITH_CACHE__
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef uintptr_t ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef BPlusStore<KeyType, ValueType, NoCache<ObjectUIDType, NoCacheObject, DataNodeType, IndexNodeType>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree);
            ptrTree.template init<DataNodeType>();

            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 10000000, 20);
        }
#else //__TREE_WITH_CACHE__
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;


            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, VolatileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024);
            ptrTree.template init<DataNodeType>();

            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 1000000, 12);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, FileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);
            ptrTree.template init<DataNodeType>();

            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 1000000, 12);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, PMemStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
#ifndef _MSC_VER
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, PMEM_STORAGE_PATH);
            ptrTree.template init<DataNodeType>();
            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 1000000, 12);
#endif //_MSC_VER
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;


            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, VolatileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024);
            ptrTree.template init<DataNodeType>();

            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 1000000, 12);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, FileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);
            ptrTree.template init<DataNodeType>();

            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 1000000, 12);
        }
        {
            typedef int KeyType;
            typedef int ValueType;
            typedef ObjectFatUID ObjectUIDType;

            typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
            typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

            typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
            typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

            typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, PMemStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
#ifndef _MSC_VER
            BPlusStoreType ptrTree(nDegree, 100, 1024, 10ULL * 1024 * 1024 * 1024, PMEM_STORAGE_PATH);
            ptrTree.template init<DataNodeType>();
            threaded_test<BPlusStoreType>(&ptrTree, nDegree, 1000000, 6);
#endif //_MSC_VER
        }
#endif //__TREE_WITH_CACHE__

        std::cout << std::endl;
    }
#endif //__CONCURRENT__
}

void quick_test()
{
    for (size_t idx = 0; idx < 5; idx++) {
        test_for_ints();
        test_for_string();
        test_for_threaded();
    }
}

template <typename BPlusStoreType>
void fptree_test(BPlusStoreType* ptrTree, size_t nMaxNumber)
{
    std::vector<int> random_numbers(nMaxNumber);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), 1); // Fill vector with 1 to 5,000,000    
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr++)
    {
        ptrTree->insert(random_numbers[nCntr], random_numbers[nCntr]);
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> insert [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
 
    std::this_thread::sleep_for(std::chrono::seconds(10));

    begin = std::chrono::steady_clock::now();

    ptrTree->flush();

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> flush [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(10));

    begin = std::chrono::steady_clock::now();

    for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr++)
    {
        int32_t nValue = 0;
        ErrorCode ec = ptrTree->search(random_numbers[nCntr], nValue);

        assert(nValue == random_numbers[nCntr]);
    }

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> search [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(10));
return;
ptrTree->flush();

    begin = std::chrono::steady_clock::now();

    for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr++)
    {
        int32_t nValue = 0;
        ErrorCode ec = ptrTree->remove(random_numbers[nCntr]);

        assert(nValue == random_numbers[nCntr]);
    }

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> delete [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
}

void fptree_bm()
{
#ifdef __TREE_WITH_CACHE__
    typedef int32_t KeyType;
    typedef int32_t ValueType;

    typedef ObjectFatUID ObjectUIDType;

    typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
    typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

    //typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
    //typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

    typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
    typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

    //typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, FileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
    //BPlusStoreType ptrTree(24, 1024, 512, 10ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);

    //typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, VolatileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
    //BPlusStoreType ptrTree(24, 1024, 4096, 10ULL * 1024 * 1024 * 1024);

    typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, PMemStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;

    // Single-threaded test
    {
        size_t nMaxNumber = 5000000;
	
        for (size_t nDegree = 2000; nDegree < 4000; nDegree = nDegree + 100)
        {
		//break;
            size_t nInternalNodeSize = (nDegree - 1) * sizeof(ValueType) + nDegree * sizeof(ObjectUIDType) + sizeof(int*);
            size_t nTotalInternalNodes = nMaxNumber / nDegree;
            //size_t nMemoryOfNodes = nTotalNodes * nNodeSize;
            //size_t nMemoryOfData = nMaxNumber * sizeof(KeyType);
            size_t nTotalMemory = nTotalInternalNodes * nInternalNodeSize;
            size_t nTotalMemoryInMB = nTotalMemory / (1024 * 1024);

            size_t nBlockSize = nInternalNodeSize > 256 ? 256 : 128;

            std::cout
                << "Order = " << nDegree
                << ", Total Memory (B) = " << nTotalMemory
                << ", Total Memory (MB) = " << nTotalMemoryInMB
                << ", Block Size = " << nBlockSize
                << std::endl;

            for (size_t nCntr = 0; nCntr < 1; nCntr++)
            {
                //BPlusStoreType ptrTree(nDegree, nTotalMemoryInMB, nBlockSize, 25ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);
                BPlusStoreType ptrTree(nDegree, 5* nTotalMemoryInMB, nBlockSize, 120ULL * 1024 * 1024 * 1024, PMEM_STORAGE_PATH_II);
                ptrTree.init<DataNodeType>();

                std::cout << "Iteration = " << nCntr + 1 << std::endl;
                fptree_test<BPlusStoreType>(&ptrTree, nMaxNumber);
                std::this_thread::sleep_for(std::chrono::seconds(10));
                //break;
            }
	    std::cout << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(10));
            //break;
        }
    }
return;
#ifdef __CONCURRENT__
    // Multi-threaded test
    {
        size_t nMaxNumber = 50000000;
        //size_t nMaxNumber = 100000;
        for (size_t nDegree = 1000; nDegree < 2001; nDegree = nDegree + 100)
        {
            size_t nInternalNodeSize = (nDegree - 1) * sizeof(ValueType) + nDegree * sizeof(ObjectUIDType) + sizeof(int*);
            size_t nTotalInternalNodes = nMaxNumber / nDegree;
            //size_t nMemoryOfNodes = nTotalNodes * nNodeSize;
            //size_t nMemoryOfData = nMaxNumber * sizeof(KeyType);
            size_t nTotalMemory = nTotalInternalNodes * nInternalNodeSize;
            size_t nTotalMemoryInMB = nTotalMemory / (1024 * 1024);

            size_t nBlockSize = nInternalNodeSize > 256 ? 256 : 128;

            std::cout
                << "Order = " << nDegree
                << ", Total Memory (B) = " << nTotalMemory
                << ", Total Memory (MB) = " << nTotalMemoryInMB
                << ", Block Size = " << nBlockSize
                << std::endl;

            for (size_t nCntr = 0; nCntr < 1; nCntr++)
            {
                //BPlusStoreType ptrTree(nDegree, nTotalMemoryInMB, nBlockSize, 25ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);
                BPlusStoreType ptrTree(nDegree, nTotalMemoryInMB, nBlockSize, 120ULL * 1024 * 1024 * 1024, PMEM_STORAGE_PATH_II);
                ptrTree.init<DataNodeType>();

                std::cout << "Iteration = " << nCntr + 1 << std::endl;
                fptree_threaded_test<BPlusStoreType>(&ptrTree, nMaxNumber, 12);
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
	    std::cout << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
    }
#endif //__CONCURRENT__
#endif //__TREE_WITH_CACHE__

}

/*int main(int argc, char* argv[])
{
    //fptree_bm();
    quick_test();
    return 0;

    typedef int KeyType;
    typedef int ValueType;

#ifdef __TREE_WITH_CACHE__
    typedef ObjectFatUID ObjectUIDType;

    typedef DataNodeROpt<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
    typedef IndexNodeROpt<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;
    
    //typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
    //typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

    typedef LRUCacheObject<TypeMarshaller, DataNodeType, IndexNodeType> ObjectType;
    typedef IFlushCallback<ObjectUIDType, ObjectType> ICallback;

    //typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, FileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
    //BPlusStoreType ptrTree(24, 1024, 512, 10ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);
    
    typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, VolatileStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
    BPlusStoreType ptrTree(24, 1024, 1024, 10ULL * 1024 * 1024 * 1024);
    
    //typedef BPlusStore<ICallback, KeyType, ValueType, LRUCache<ICallback, PMemStorage<ICallback, ObjectUIDType, LRUCacheObject, TypeMarshaller, DataNodeType, IndexNodeType>>> BPlusStoreType;
    //BPlusStoreType ptrTree(48, 4096 ,512 , 10ULL * 1024 * 1024 * 1024, FILE_STORAGE_PATH);

    ptrTree.init<DataNodeType>();
#else //__TREE_WITH_CACHE__
    typedef int KeyType;
    typedef int ValueType;
    typedef uintptr_t ObjectUIDType;

    typedef DataNode<KeyType, ValueType, ObjectUIDType, TYPE_UID::DATA_NODE_INT_INT> DataNodeType;
    typedef IndexNode<KeyType, ValueType, ObjectUIDType, DataNodeType, TYPE_UID::INDEX_NODE_INT_INT> IndexNodeType;

    typedef BPlusStore<KeyType, ValueType, NoCache<ObjectUIDType, NoCacheObject, DataNodeType, IndexNodeType>> BPlusStoreType;
    BPlusStoreType ptrTree(24);
    ptrTree.init<DataNodeType>();
#endif //__TREE_WITH_CACHE__

    size_t nTotalEntries = 50000000;
    std::vector<int> random_numbers(nTotalEntries);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), 1); // Fill vector with 1 to 5,000,000
    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (size_t nCntr = 0; nCntr < nTotalEntries; nCntr = nCntr++)
    {
        ptrTree.insert(random_numbers[nCntr], random_numbers[nCntr]);
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> insert [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

#ifdef __TREE_WITH_CACHE__
    begin = std::chrono::steady_clock::now();

    ptrTree.flush();

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> flush [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
#endif //__TREE_WITH_CACHE__

    begin = std::chrono::steady_clock::now();

    for (size_t nCntr = 0; nCntr <= nTotalEntries; nCntr = nCntr + 2)
    {
        ValueType nValue = 0;
        ErrorCode ec = ptrTree.search(random_numbers[nCntr], nValue);

        assert(nValue == random_numbers[nCntr]);
    }

    end = std::chrono::steady_clock::now();
    std::cout
        << ">> search [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;

    for (size_t nCntr = 0; nCntr < nTotalEntries; nCntr++)
    {
        ErrorCode ec = ptrTree.remove(nCntr);
    }

    for (size_t nCntr = 0; nCntr < nTotalEntries; nCntr++)
    {
        ValueType nValue = 0;
        ErrorCode ec = ptrTree.search(nCntr, nValue);

        assert(ec == ErrorCode::KeyDoesNotExist);
    }

    std::cout << "End.";
    return 0;
}
*/