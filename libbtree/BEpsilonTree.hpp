#pragma once
#include <libpmemobj.h>
#include <libpmemobj/base.h>
#include <libpmemobj/pool_base.h>
#include <libpmemobj/atomic_base.h>
#include "ErrorCodes.h"
#include "Operations.h"
#include "nvm.hpp"
#include "DiskLeafNode.hpp"
#include <iostream>
#include <algorithm>
#include <utility>
#include <iterator>
#include <memory>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <map>
#include <fstream>
#include <queue>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <unordered_map>
#include <thread>
#include <list>
#include <optional>
#include <sstream>

using namespace std;

template <typename KeyType, typename ValueType>
class BEpsilonTree
{
public:
    TOID(PersistentNode) persistentRoot;
    PMEMobjpool *pmemPoolHandle = nullptr; // NVM pool

    uint32_t m_nDegree;
    std::string logFilename = getPlatformPath("nvm_tree_test1.log");
    int opCounter = 0;
    int checkpointFrequency = 5;
    bool isReplaying = false;
    uint64_t poolUUID = 0;
    int nextLeafFileID = 0;

    // DRAM read buffer
    std::unordered_map<KeyType, ValueType> readCache;
    std::list<KeyType> lruList;
    size_t maxReadCacheSize = 100;

    BEpsilonTree(int degree, const std::string &filename, int checkpointFreq = -1)
    {
        m_nDegree = degree;
        if (checkpointFreq > 0)
            this->checkpointFrequency = checkpointFreq;

        // PMEM POOL INIT
        const std::string pmemPath = getPlatformPath("shared_buffer_pool.pmem");
        std::cout << "[DEBUG] Trying to open PMEM pool at: " << pmemPath << "\n";
        initializePMEMPool(pmemPath);

        // Load persistentRoot from PMEMRoot
        TOID(PMEMRoot)
        pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        persistentRoot = D_RW(pmemRoot)->persistentRoot;

        if (TOID_IS_NULL(persistentRoot))
        {
            std::cout << "[INIT] Allocating new persistent root node...\n";
            persistentRoot = allocatePersistentNode(true); // Create a leaf root

            // Store into PMEMRoot for future use
            D_RW(pmemRoot)->persistentRoot = persistentRoot;
            pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));
        }
        else
        {
            std::cout << "[INIT] Loaded persistentRoot from PMEM pool.\n";
        }

        // Load Binary WAL if it exists
        replayBinaryWAL();
    }

    ~BEpsilonTree()
    {
        if (opCounter > 0)
        {
            checkpoint();
        }

        const std::string binWal = getPlatformPath("nvm_tree_bin.wal");
        const std::string bakName = getPlatformPath("wal_bin_backup") + timestampename() + ".bak";

        std::ifstream src(binWal, std::ios::binary);
        std::ofstream dst(bakName, std::ios::binary);
        if (src && dst)
        {
            dst << src.rdbuf();
        }
        src.close();
        dst.close();

        std::ofstream clear(binWal, std::ios::trunc | std::ios::binary);
        clear.close();

        if (pmemPoolHandle)
        {
            pmemobj_close(pmemPoolHandle);
            pmemPoolHandle = nullptr;
        }
    }

private:
    void replayBinaryWAL()
    {
        const std::string walPath = getPlatformPath("nvm_tree_bin.wal");
        std::ifstream walBin(walPath, std::ios::binary);
        if (!walBin.is_open())
        {
            std::cout << "[WAL] No WAL file to replay.\n";
            return;
        }

        std::cout << "[WAL] Replaying binary WAL from: " << walPath << "\n";
        isReplaying = true;

        while (!walBin.eof())
        {
            uint8_t opCode;
            KeyType key;
            ValueType value;

            walBin.read(reinterpret_cast<char *>(&opCode), sizeof(opCode));
            if (walBin.eof())
                break;

            walBin.read(reinterpret_cast<char *>(&key), sizeof(KeyType));
            Operations op = static_cast<Operations>(opCode);

            if (op == Operations::Insert || op == Operations::Update)
            {
                walBin.read(reinterpret_cast<char *>(&value), sizeof(ValueType));
            }

            switch (op)
            {
            case Operations::Insert:
                insert(key, value);
                break;
            case Operations::Update:
                update(key, value);
                break;
            case Operations::Delete:
                remove(key);
                break;
            case Operations::Upsert:
                upsert(key, value);
                break;
            default:
                std::cerr << "[WARN] Unknown or unhandled WAL op: " << static_cast<int>(op) << "\n";
                break;
            }
        }

        walBin.close();
        isReplaying = false;
    }

    void initializePMEMPool(const std::string &pmemPath)
    {
#ifdef _WIN32
        std::wstring pmemPathW(pmemPath.begin(), pmemPath.end());
        pmemPoolHandle = pmemobj_openW(pmemPathW.c_str(), LAYOUT_NAME);
#else
        pmemPoolHandle = pmemobj_open(pmemPath.c_str(), LAYOUT_NAME);
#endif

        if (!pmemPoolHandle)
        {
            std::cout << "[DEBUG] Pool not found. Creating new PMEM pool...\n";
#ifdef _WIN32
            pmemPoolHandle = pmemobj_createW(pmemPathW.c_str(), LAYOUT_NAME,
                                             1024 * 1024 * 1024, 0666);
#else
            pmemPoolHandle = pmemobj_create(pmemPath.c_str(), LAYOUT_NAME,
                                            1024 * 1024 * 1024, 0666);
#endif

            if (!pmemPoolHandle)
            {
                std::cerr << "[ERROR] Failed to create PMEM pool! Exiting.\n";
                perror("pmemobj_create");
                exit(1);
            }
            std::cout << "[NVM] New PMEM pool created successfully after backup.\n";

            // Initialize the root object
            TOID(PMEMRoot)
            root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            auto *ptr = D_RW(root);

            // Zero out the root's fields
            ptr->messageCount = 0;
            for (int i = 0; i < MAX_NVM_MESSAGES; ++i)
            {
                ptr->messages[i] = TOID_NULL(message);
            }
            ptr->nodeFrequencyCount = 0;
            ptr->nodeMessageMapCount = 0;
            ptr->messageToNodeMapCount = 0;

            pmemobj_persist(pmemPoolHandle, ptr, sizeof(PMEMRoot));
            std::cout << "[NVM] New PMEM pool created successfully.\n";
        }
        else
        {
            std::cout << "[NVM] Existing PMEM pool opened successfully.\n";
        }

        TOID(PMEMRoot)
        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        PMEMoid rootOID = pmemobj_oid(D_RW(root));
        this->poolUUID = rootOID.pool_uuid_lo;
    }

    TOID(PersistentNode) allocatePersistentNode(bool isLeaf)
    {
        if (isLeaf)
        {
            // Store to SSD/HDD instead
            std::string path = allocateLeafOnDisk(getPlatformPath("leaf_storage"), nextLeafFileID++);
            if (path.empty())
            {
                std::cerr << "[ERROR] Failed to allocate SSD-based leaf node.\n";
                return TOID_NULL(PersistentNode);
            }

            // Return a dummy TOID with encoded offset = hash of path or index
            TOID(PersistentNode) dummy;
            dummy.oid.off = std::hash<std::string>{}(path); // placeholder
            dummy.oid.pool_uuid_lo = poolUUID;
            dummy.oid.off = 10000 + nextLeafFileID - 1;
            return dummy;
        }

        // NVM-backed internal node
        TOID(PersistentNode) node;
        int ret = pmemobj_alloc(pmemPoolHandle, &node.oid, sizeof(PersistentNode), 0, nullptr, nullptr);
        if (ret != 0 || TOID_IS_NULL(node))
        {
            std::cerr << "[ERROR] Failed to allocate PersistentNode in PMEM (ret=" << ret << ").\n";
            return TOID_NULL(PersistentNode);
        }

        auto *ptr = D_RW(node);
        ptr->isLeaf = 0;
        ptr->keyCount = 0;
        ptr->buffer_offset = 0;
        std::memset(ptr->keys, 0, sizeof(ptr->keys));
        std::memset(ptr->children, 0, sizeof(ptr->children));
        std::memset(ptr->buffer, 0, NODE_BUFFER_SIZE);
        pmemobj_persist(pmemPoolHandle, ptr, sizeof(PersistentNode));
        return node;
    }

    bool isSSDLeaf(uint64_t node_id)
    {
        return node_id >= 10000 && node_id <= 10000 + nextLeafFileID;
    }

    std::string getLeafFilePathFromID(uint64_t node_id)
    {
        int fileID = node_id - 10000;
        return getPlatformPath("leaf_storage/leaf_" + std::to_string(fileID) + ".bin");
    }

    ErrorCode appendMessageToPersistentNode(TOID(PersistentNode) node, const message &msg)
    {
        if (TOID_IS_NULL(node))
            return ErrorCode::Error;

        auto *n = D_RW(node);
        size_t msgSize = sizeof(message);

        if (n->buffer_offset + msgSize > NODE_BUFFER_SIZE)
        {
            std::cerr << "[WARN] PersistentNode buffer full. Cannot append.\n";
            return ErrorCode::Error;
        }

        // Copy message into buffer at current offset
        std::memcpy(n->buffer + n->buffer_offset, &msg, msgSize);
        pmemobj_persist(pmemPoolHandle, n->buffer + n->buffer_offset, msgSize);

        // Update offset
        n->buffer_offset += msgSize;
        pmemobj_persist(pmemPoolHandle, &n->buffer_offset, sizeof(size_t));

        return ErrorCode::Success;
    }

    void incrementNodeFrequency(uint64_t node_id)
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->nodeFrequencyCount; ++i)
        {
            auto *entry = D_RW(auxPtr->nodeFrequencyMap[i]);
            if (entry->node_id == node_id)
            {
                entry->frequency += 1;
                pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeFrequencyEntry));
                return;
            }
        }

        if (auxPtr->nodeFrequencyCount < 128)
        {
            TOID(NodeFrequencyEntry)
            newEntry;
            if (pmemobj_alloc(pmemPoolHandle, &newEntry.oid, sizeof(NodeFrequencyEntry), 0, nullptr, nullptr) == 0)
            {
                D_RW(newEntry)->node_id = node_id;
                D_RW(newEntry)->frequency = 1;
                pmemobj_persist(pmemPoolHandle, D_RW(newEntry), sizeof(NodeFrequencyEntry));
                auxPtr->nodeFrequencyMap[auxPtr->nodeFrequencyCount++] = newEntry;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot));
            }
        }
    }

    ErrorCode insertToNVMSharedBuffer(Operations op, const KeyType &key, const ValueType &value = ValueType{})
    {
        if (!pmemPoolHandle)
            return ErrorCode::Error;

        TOID(PMEMRoot)
        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *rootPtr = D_RW(root);

        // Search for existing message on the same key
        int existingIndex = -1;
        for (int i = 0; i < rootPtr->messageCount; ++i)
        {
            message *msgPtr = D_RW(rootPtr->messages[i]);
            if (msgPtr->key_size == sizeof(KeyType) &&
                std::memcmp(msgPtr->key_data, &key, sizeof(KeyType)) == 0)
            {
                existingIndex = i;
                break;
            }
        }

        // COALESCING LOGIC
        if (existingIndex != -1)
        {
            message *msgPtr = D_RW(rootPtr->messages[existingIndex]);
            Operations prevOp = static_cast<Operations>(msgPtr->opCode);

            if ((prevOp == Operations::Insert || prevOp == Operations::Upsert) && op == Operations::Delete)
            {
                // Remove message from buffer
                pmemobj_free(&rootPtr->messages[existingIndex].oid);
                for (int j = existingIndex + 1; j < rootPtr->messageCount; ++j)
                    rootPtr->messages[j - 1] = rootPtr->messages[j];
                rootPtr->messageCount--;
                pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));

                // Clean metadata
                removeKeyFromAllNodeMessageMaps(key);
                removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));

                return ErrorCode::Success;
            }

            if (prevOp == Operations::Insert && op == Operations::Update)
            {
                // Keep as Insert, just update value
                msgPtr->val_size = sizeof(ValueType);
                std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
                pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
                return ErrorCode::Success;
            }

            // Regular overwrite (Insert/Update/Upsert)
            msgPtr->opCode = static_cast<uint8_t>(op);
            msgPtr->key_size = sizeof(KeyType);
            msgPtr->val_size = (op == Operations::Delete) ? 0 : sizeof(ValueType);
            std::memcpy(msgPtr->key_data, &key, sizeof(KeyType));
            if (op != Operations::Delete)
            {
                std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
            }
            pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));

            // FIX: Restore node mappings in case they were flushed
            auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
            if (!nodeIdOpt.has_value())
            {
                if (isSSDLeaf(persistentRoot.oid.off))
                {
                    std::cerr << "[BUFFERING] ERROR: Tried to find target internal node while root is SSD leaf. Aborting.\n";
                    return ErrorCode::Error;
                }

                TOID(PersistentNode) bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
                if (!TOID_IS_NULL(bufferTarget))
                {
                    uint64_t node_id = getNodeIDFromPersistentNode(bufferTarget);
                    addKeyToNodeMessageMap(node_id, key);
                    insertToMessageToNodeMap(static_cast<uint64_t>(key), node_id);
                    incrementNodeFrequency(node_id);
                    // std::cout << "[DEBUG] Mapped key " << key << " to node " << node_id << "\n";
                }
                else
                {
                    std::cerr << "[WARNING] Could not map key " << key << " to any internal node!\n";
                }
            }

            return ErrorCode::Success;
        }

        // HANDLE INSERTION OF NEW DELETE
        if (op == Operations::Delete)
        {
            removeKeyFromAllNodeMessageMaps(key);
            removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));
        }

        // FLUSH IF FULL
        int flushRetries = 0;
        while (rootPtr->messageCount >= MAX_NVM_MESSAGES)
        {
            // std::cout << "[INSERT RETRY] Buffer full or key " << key << " still present -> flushing...\n";
            if (++flushRetries > 3)
            {
                std::cerr << "[FLUSH] Too many flush retries — aborting insert.\n";
                return ErrorCode::Error;
            }
            auto flushResult = flushMostBufferedNode();
            if (flushResult != ErrorCode::Success)
            {
                std::cerr << "[ERROR] Failed to flush any node. Shared buffer stuck.\n";
                return ErrorCode::Error;
            }

            // Refresh pointers after flush
            root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            rootPtr = D_RW(root);
        }

        removeKeyFromAllNodeMessageMaps(key);
        removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));

        // ALLOCATE NEW MESSAGE
        TOID(message)
        msg;
        int alloc_status = pmemobj_alloc(pmemPoolHandle, &msg.oid, sizeof(message), 0, nullptr, nullptr);
        if (alloc_status != 0 || TOID_IS_NULL(msg))
        {
            std::cerr << "[NVM] Failed to allocate message in PMEM (status = " << alloc_status << ").\n";
            return ErrorCode::Error;
        }

        auto *msgPtr = D_RW(msg);
        msgPtr->opCode = static_cast<uint8_t>(op);
        msgPtr->key_size = sizeof(KeyType);
        msgPtr->val_size = sizeof(ValueType);
        std::memcpy(msgPtr->key_data, &key, sizeof(KeyType));
        std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
        pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));

        rootPtr->messages[rootPtr->messageCount++] = msg;
        pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));

        // Recompute and fix node assignment (especially after SSD splits)
        if (isSSDLeaf(persistentRoot.oid.off))
        {
            std::cerr << "[BUFFERING] 2 ERROR: Tried to find target internal node while root is SSD leaf. Aborting.\n";
            return ErrorCode::Error;
        }

        TOID(PersistentNode) bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (!TOID_IS_NULL(bufferTarget))
        {
            uint64_t new_id = getNodeIDFromPersistentNode(bufferTarget);

            auto old_id_opt = lookupNodeIdForKey(static_cast<uint64_t>(key));

            // Always remove all old mappings unconditionally
            removeKeyFromAllNodeMessageMaps(key);
            removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));

            // Now add clean new mapping
            addKeyToNodeMessageMap(new_id, key);
            insertToMessageToNodeMap(static_cast<uint64_t>(key), new_id);
            incrementNodeFrequency(new_id);
        }
        else
        {
            std::cerr << "[WARNING] Could not map key " << key << " to any internal node!\n";
        }

        return ErrorCode::Success;
    }

    // Simple LRU read-cache update
    void updateReadCache(const KeyType &key, const ValueType &value)
    {
        // If already in cache -> move to front (MRU)
        if (readCache.find(key) != readCache.end())
        {
            lruList.remove(key);
        }
        else
        {
            // Not in cache -> check capacity
            if (readCache.size() >= maxReadCacheSize)
            {
                // Evict LRU
                KeyType lruKey = lruList.back();
                lruList.pop_back();
                readCache.erase(lruKey);
            }
        }

        // Insert or update
        lruList.push_front(key);
        readCache[key] = value;
    }

    // Create a UTC timestamp string
    std::string timestampename()
    {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utcTime;
#ifdef _WIN32
        gmtime_s(&utcTime, &time); // Windows
#else
        gmtime_r(&time, &utcTime); // Linux/Unix
#endif
        std::stringstream timestamp;
        timestamp << std::put_time(&utcTime, "%Y-%m-%d_%H-%M-%S");
        return timestamp.str();
    }

    void checkpoint()
    {
        if (TOID_IS_NULL(persistentRoot))
            return;

        // Backup binary WAL
        std::string backupFilename = getPlatformPath("wal_backup") + timestampename() + ".bak";
        std::ifstream src(logFilename, std::ios::binary);
        std::ofstream dst(backupFilename, std::ios::binary);
        if (src && dst)
        {
            dst << src.rdbuf();
        }
        src.close();
        dst.close();

        // Clear the textual WAL
        std::ofstream clearLog(logFilename, std::ios::trunc);
        clearLog.close();
    }

    void maybeCheckpoint()
    {
        if (isReplaying)
            return;

        if (++opCounter >= checkpointFrequency)
        {
            checkpoint();
            opCounter = 0;
        }
    }

    void logOperationBinary(Operations op, const KeyType &key, const ValueType &value = ValueType{})
    {
        if (isReplaying)
            return;
        std::ofstream log(getPlatformPath("nvm_tree_bin.wal"),
                          std::ios::binary | std::ios::app);
        if (!log.is_open())
        {
            std::cerr << "Failed to open binary WAL.\n";
            return;
        }

        uint8_t opCode = static_cast<uint8_t>(op);
        log.write(reinterpret_cast<const char *>(&opCode), sizeof(opCode));
        log.write(reinterpret_cast<const char *>(&key), sizeof(KeyType));

        if (op == Operations::Insert || op == Operations::Update)
        {
            log.write(reinterpret_cast<const char *>(&value), sizeof(ValueType));
        }
        log.close();
    }

public:
    // DELETE OP
    ErrorCode remove(KeyType key)
    {
        if (isReplaying)
            return ErrorCode::Success;

        logOperationBinary(Operations::Delete, key);

        if (TOID_IS_NULL(persistentRoot))
        {
            std::cerr << "[REMOVE] No persistent root exists.\n";
            return ErrorCode::KeyDoesNotExist;
        }

        if (isSSDLeaf(persistentRoot.oid.off))
        {
            std::string path = getLeafFilePathFromID(persistentRoot.oid.off);
            auto leaf = std::make_unique<SerializedLeafNode>();

            if (!std::filesystem::exists(path))
            {
                goto buffered_remove;
            }

            if (!loadLeafFromDisk(path, *leaf))
            {
                std::cerr << "[REMOVE] Failed to load SSD root leaf.\n";
                return ErrorCode::Error;
            }

            // Search for the key in the leaf
            int pos = 0;
            while (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] < key)
                ++pos;

            if (pos >= static_cast<int>(leaf->keyCount) || leaf->keys[pos] != key)
            {
                std::cerr << "[REMOVE] Key " << key << " not found in SSD root.\n";
                return ErrorCode::KeyDoesNotExist;
            }

            // Shift entries left to delete
            for (int i = pos; i < static_cast<int>(leaf->keyCount) - 1; ++i)
            {
                leaf->keys[i] = leaf->keys[i + 1];
                leaf->values[i] = leaf->values[i + 1];
            }

            leaf->keyCount--;

            if (!saveLeafToDisk(path, *leaf))
            {
                std::cerr << "[REMOVE] Failed to save SSD leaf after deletion.\n";
                return ErrorCode::Error;
            }

            if (leaf->keyCount < (MAX_KEYS_PER_NODE / 2))
            {
                TOID(PersistentNode) parent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), persistentRoot);
                if (!TOID_IS_NULL(parent))
                {
                    handleUnderflowPersistent(parent, persistentRoot);
                }
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }
    buffered_remove:
        // Internal tree -> buffer the delete message
        TOID(PersistentNode) bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget))
        {
            std::cerr << "[REMOVE] Could not find internal node to buffer delete.\n";
            return ErrorCode::Error;
        }

        ErrorCode res = insertToNVMSharedBuffer(Operations::Delete, key);
        if (res != ErrorCode::Success)
            return res;

        uint64_t node_id = getNodeIDFromPersistentNode(bufferTarget);
        addKeyToNodeMessageMap(node_id, key);
        insertToMessageToNodeMap(static_cast<uint64_t>(key), node_id);
        incrementNodeFrequency(node_id);

        maybeCheckpoint();

        // Check for underflow and fix if needed
        auto *nodePtr = D_RW(bufferTarget);
        if (nodePtr->keyCount < (m_nDegree / 2))
        {
            TOID(PersistentNode) parent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), bufferTarget);
            if (!TOID_IS_NULL(parent))
            {
                handleUnderflowPersistent(parent, bufferTarget);
            }
            else
            {
                std::cerr << "[REMOVE] No parent found for internal node " << bufferTarget.oid.off << " during underflow check.\n";
            }
        }

        return ErrorCode::Success;
    }

    ErrorCode mergeSSDLeaves(uint64_t leftID, uint64_t rightID)
    {
        std::string leftPath = getLeafFilePathFromID(leftID);
        std::string rightPath = getLeafFilePathFromID(rightID);

        if (!std::filesystem::exists(leftPath) || !std::filesystem::exists(rightPath))
        {
            std::cerr << "[MERGE ERROR] One or both SSD leaf files missing\n";
            return ErrorCode::Error;
        }

        SerializedLeafNode leftLeaf, rightLeaf;

        if (!loadLeafFromDisk(leftPath, leftLeaf) || !loadLeafFromDisk(rightPath, rightLeaf))
        {
            std::cerr << "[MERGE ERROR] Failed to load SSD leaves from disk\n";
            return ErrorCode::Error;
        }

        if (leftLeaf.keyCount + rightLeaf.keyCount > MAX_KEYS_PER_NODE)
        {
            std::cerr << "[MERGE ERROR] Combined key count exceeds capacity\n";
            return ErrorCode::Error;
        }

        for (int i = 0; i < static_cast<int>(rightLeaf.keyCount); ++i)
        {
            leftLeaf.keys[leftLeaf.keyCount + i] = rightLeaf.keys[i];
            leftLeaf.values[leftLeaf.keyCount + i] = rightLeaf.values[i];
        }

        leftLeaf.keyCount += rightLeaf.keyCount;

        if (!saveLeafToDisk(leftPath, leftLeaf))
        {
            std::cerr << "[MERGE ERROR] Failed to persist merged SSD leaf\n";
            return ErrorCode::Error;
        }

        // Remove the now-merged right leaf file
        std::error_code ec;
        std::filesystem::remove(rightPath, ec);
        if (ec)
        {
            std::cerr << "[MERGE WARNING] Failed to delete SSD file: " << rightPath << "\n";
        }

        return ErrorCode::Success;
    }

    ErrorCode handleUnderflowPersistent(TOID(PersistentNode) parent, TOID(PersistentNode) node)
    {
        auto *parentPtr = D_RW(parent);
        if (!parentPtr)
            return ErrorCode::Error;

        int index = -1;
        for (int i = 0; i <= parentPtr->keyCount; ++i)
        {
            if (parentPtr->children[i] == node.oid.off)
            {
                index = i;
                break;
            }
        }

        if (index == -1)
            return ErrorCode::Error;

        TOID(PersistentNode) left = TOID_NULL(PersistentNode);
        TOID(PersistentNode) right = TOID_NULL(PersistentNode);

        if (index > 0)
        {
            left.oid.off = parentPtr->children[index - 1];
            left.oid.pool_uuid_lo = poolUUID;
        }
        if (index + 1 <= parentPtr->keyCount)
        {
            right.oid.off = parentPtr->children[index + 1];
            right.oid.pool_uuid_lo = poolUUID;
        }

        auto *nodePtr = D_RW(node);
        auto *leftPtr = TOID_IS_NULL(left) ? nullptr : D_RW(left);
        auto *rightPtr = TOID_IS_NULL(right) ? nullptr : D_RW(right);

        // Try borrowing from left sibling
        if (leftPtr && leftPtr->keyCount > m_nDegree / 2)
        {
            int last = leftPtr->keyCount - 1;
            for (int j = nodePtr->keyCount; j > 0; --j)
            {
                nodePtr->keys[j] = nodePtr->keys[j - 1];
                nodePtr->children[j] = nodePtr->children[j - 1];
            }
            nodePtr->keys[0] = parentPtr->keys[index - 1];
            nodePtr->children[0] = leftPtr->children[last];
            nodePtr->keyCount++;

            leftPtr->keyCount--;
            parentPtr->keys[index - 1] = leftPtr->keys[last];

            pmemobj_persist(pmemPoolHandle, nodePtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, leftPtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
            return ErrorCode::Success;
        }

        // Try borrowing from right sibling
        if (rightPtr && rightPtr->keyCount > m_nDegree / 2)
        {
            nodePtr->keys[nodePtr->keyCount] = parentPtr->keys[index];
            nodePtr->children[nodePtr->keyCount] = rightPtr->children[0];
            nodePtr->keyCount++;

            for (int j = 0; j < rightPtr->keyCount - 1; ++j)
            {
                rightPtr->keys[j] = rightPtr->keys[j + 1];
                rightPtr->children[j] = rightPtr->children[j + 1];
            }
            rightPtr->keyCount--;
            rightPtr->children[rightPtr->keyCount] = rightPtr->children[rightPtr->keyCount + 1];

            parentPtr->keys[index] = rightPtr->keys[0];

            pmemobj_persist(pmemPoolHandle, nodePtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, rightPtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
            return ErrorCode::Success;
        }

        // If borrowing failed, merge
        if (!TOID_IS_NULL(left) && index - 1 >= 0)
        {
            return mergeNodes(parent, index - 1);
        }
        else if (!TOID_IS_NULL(right) && index + 1 <= parentPtr->keyCount)
        {
            return mergeNodes(parent, index);
        }
        else
        {
            std::cerr << "[UNDERFLOW] No valid sibling to merge with!\n";
            return ErrorCode::Error;
        }
    }

    ErrorCode mergeNodes(TOID(PersistentNode) parent, int index)
    {
        auto *parentPtr = D_RW(parent);

        if (index < 0 || index + 1 > parentPtr->keyCount)
            return ErrorCode::Error;

        TOID(PersistentNode) left, right;
        left.oid.off = parentPtr->children[index];
        right.oid.off = parentPtr->children[index + 1];
        left.oid.pool_uuid_lo = poolUUID;
        right.oid.pool_uuid_lo = poolUUID;

        if (isSSDLeaf(left.oid.off) || isSSDLeaf(right.oid.off))
        {
            uint64_t leftID = parentPtr->children[index];
            uint64_t rightID = parentPtr->children[index + 1];
            return mergeSSDLeaves(leftID, rightID);
        }

        auto *leftPtr = D_RW(left);
        auto *rightPtr = D_RW(right);
        if (!leftPtr || !rightPtr)
            return ErrorCode::Error;

        if (!leftPtr->isLeaf)
        {
            leftPtr->keys[leftPtr->keyCount] = parentPtr->keys[index];
            leftPtr->keyCount++;
        }

        for (int i = 0; i < rightPtr->keyCount; ++i)
        {
            leftPtr->keys[leftPtr->keyCount] = rightPtr->keys[i];
            leftPtr->children[leftPtr->keyCount] = rightPtr->children[i];
            leftPtr->keyCount++;
        }

        if (!leftPtr->isLeaf)
        {
            leftPtr->children[leftPtr->keyCount] = rightPtr->children[rightPtr->keyCount];
        }

        for (int i = index; i < parentPtr->keyCount - 1; ++i)
        {
            parentPtr->keys[i] = parentPtr->keys[i + 1];
            parentPtr->children[i + 1] = parentPtr->children[i + 2];
        }
        parentPtr->keyCount--;
        if (parentPtr->keyCount == 0)
        {
            for (int i = 0; i <= MAX_KEYS_PER_NODE; ++i)
                parentPtr->children[i] = 0;
            for (int i = 0; i < MAX_KEYS_PER_NODE; ++i)
                parentPtr->keys[i] = 0;

            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
        }

        uint64_t left_id = getNodeIDFromPersistentNode(left);
        uint64_t right_id = getNodeIDFromPersistentNode(right);
        auto keys = getBufferedKeysForNode(right_id);
        for (auto &k : keys)
        {
            moveBufferedKeyBetweenNodes(right_id, left_id, k);
            insertToMessageToNodeMap(k, left_id);
        }

        int oldKeyCnt = parentPtr->keyCount;
        pmemobj_free(&right.oid);
        parentPtr->children[oldKeyCnt + 1] = 0;
        pmemobj_persist(pmemPoolHandle, leftPtr, sizeof(PersistentNode));
        pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));

        if (parent.oid.off == persistentRoot.oid.off && parentPtr->keyCount == 0)
        {
            TOID(PersistentNode) newRoot;
            newRoot.oid.off = parentPtr->children[0];
            newRoot.oid.pool_uuid_lo = poolUUID;

            pmemobj_free(&persistentRoot.oid);
            persistentRoot = newRoot;

            TOID(PMEMRoot)
            pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(pmemRoot)->persistentRoot = persistentRoot;
            pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));
        }
        else if (parent.oid.off != persistentRoot.oid.off &&
                 parentPtr->keyCount < (m_nDegree / 2))
        {
            TOID(PersistentNode) grandparent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), parent);
            if (!TOID_IS_NULL(grandparent))
            {
                return handleUnderflowPersistent(grandparent, parent);
            }
        }

        return ErrorCode::Success;
    }

    // UPDATE OP
    ErrorCode update(KeyType key, ValueType newValue)
    {
        if (isReplaying)
            return ErrorCode::Success;

        logOperationBinary(Operations::Update, key, newValue);

        // Tree is empty
        if (TOID_IS_NULL(persistentRoot))
        {
            std::cerr << "[UPDATE] Cannot update — tree is empty.\n";
            return ErrorCode::KeyDoesNotExist;
        }

        // SSD leaf root
        if (isSSDLeaf(persistentRoot.oid.off))
        {
            std::string path = getLeafFilePathFromID(persistentRoot.oid.off);
            auto leaf = std::make_unique<SerializedLeafNode>();

            if (!std::filesystem::exists(path))
            {
                goto buffered_update;
                // std::cerr << "[UPDATE] SSD leaf file missing for root — cannot update key " << key << ".\n";
                // return ErrorCode::KeyDoesNotExist;
            }

            if (!loadLeafFromDisk(path, *leaf))
            {
                std::cerr << "[UPDATE] Failed to load SSD root leaf.\n";
                return ErrorCode::Error;
            }

            // Search for key
            int pos = 0;
            while (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] < key)
                ++pos;

            if (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] == key)
            {
                // Existing key — update value only
                leaf->values[pos] = newValue;

                if (!saveLeafToDisk(path, *leaf))
                {
                    std::cerr << "[UPDATE] Failed to save SSD root leaf.\n";
                    return ErrorCode::Error;
                }

                maybeCheckpoint();
                return ErrorCode::Success;
            }
            else
            {
                std::cerr << "[UPDATE] Key " << key << " not found in SSD root.\n";
                return ErrorCode::KeyDoesNotExist;
            }
        }

        // Buffer update in internal tree
    buffered_update:
        TOID(PMEMRoot)
        pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        persistentRoot = D_RW(pmemRoot)->persistentRoot;

        TOID(PersistentNode) bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget))
        {
            std::cerr << "[UPDATE] ERROR: Could not find internal node to buffer into.\n";
            return ErrorCode::Error;
        }

        ErrorCode res = insertToNVMSharedBuffer(Operations::Update, key, newValue);
        if (res != ErrorCode::Success)
            return res;

        maybeCheckpoint();
        return ErrorCode::Success;
    }

    ErrorCode searchRecursive(TOID(PersistentNode) node, KeyType key, ValueType &value)
    {
        if (TOID_IS_NULL(node))
            return ErrorCode::KeyDoesNotExist;

        auto *ptr = D_RO(node);
        if (!ptr)
            return ErrorCode::Error;

        // If leaf, look for key directly
        if (ptr->isLeaf)
        {
            if (!isSSDLeaf(node.oid.off))
            {
                for (int i = 0; i < ptr->keyCount; ++i)
                {
                    if (ptr->keys[i] == key)
                    {
                        value = static_cast<ValueType>(ptr->children[i]);
                        return ErrorCode::Success;
                    }
                }
                return ErrorCode::KeyDoesNotExist;
            }
            else
            {
                // SSD-backed leaf -> load from disk
                std::string filePath = getLeafFilePathFromID(node.oid.off);
                std::ifstream file(filePath, std::ios::binary);
                if (!file)
                {
                    std::cerr << "[ERROR] Could not open SSD leaf: " << filePath << "\n";
                    return ErrorCode::Error;
                }

                auto leaf = std::make_unique<SerializedLeafNode>();
                file.read(reinterpret_cast<char *>(&leaf), sizeof(SerializedLeafNode));
                file.close();

                for (size_t i = 0; i < leaf->keyCount; ++i)
                {
                    if (leaf->keys[i] == key)
                    {
                        value = static_cast<ValueType>(leaf->values[i]);
                        return ErrorCode::Success;
                    }
                }
                return ErrorCode::KeyDoesNotExist;
            }
        }

        // Internal node -> determine child
        int i = 0;
        while (i < static_cast<int>(ptr->keyCount) && key >= ptr->keys[i])
        {
            ++i;
        }

        if (i > MAX_KEYS_PER_NODE || ptr->children[i] == 0)
        {
            std::cerr << "[ERROR] Invalid child pointer at index " << i << " in internal node.\n";
            return ErrorCode::Error;
        }

        TOID(PersistentNode) child;
        child.oid.off = ptr->children[i];
        child.oid.pool_uuid_lo = poolUUID;

        return searchRecursive(child, key, value);
    }

    // SEARCH OP
    ErrorCode search(KeyType key, ValueType &value)
    {
        if (isReplaying)
            return ErrorCode::KeyDoesNotExist;

        // 1. Check DRAM read cache
        auto cacheIt = readCache.find(key);
        if (cacheIt != readCache.end())
        {
            lruList.remove(key);
            lruList.push_front(key);
            value = cacheIt->second;
            return ErrorCode::Success;
        }

        // 2. Check NVM shared buffer (unflushed message)
        auto bufferedMessage = lookupInNVMBuffer(key);
        if (bufferedMessage.has_value())
        {
            auto [op, val] = bufferedMessage.value();
            if (op == Operations::Insert || op == Operations::Update)
            {
                value = val;
                updateReadCache(key, value);
                return ErrorCode::Success;
            }
            if (op == Operations::Delete)
            {
                return ErrorCode::KeyDoesNotExist;
            }
        }

        // 3. PersistentNode recursive search
        if (!TOID_IS_NULL(persistentRoot))
        {
            ErrorCode result = searchRecursive(persistentRoot, key, value);
            if (result == ErrorCode::Success)
            {
                updateReadCache(key, value);
            }
            return result;
        }

        return ErrorCode::KeyDoesNotExist;
    }

    // RANGE QUERY
    std::vector<std::pair<KeyType, ValueType>> rangeQuery(KeyType low, KeyType high)
    {
        std::vector<std::pair<KeyType, ValueType>> result;

        if (TOID_IS_NULL(persistentRoot))
            return result;

        // 1. Traverse persistent tree recursively
        rangeQueryRecursive(persistentRoot, low, high, result);

        // 2. Overlay messages from NVM shared buffer
        if (pmemPoolHandle)
        {
            TOID(PMEMRoot)
            root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            auto *rootPtr = D_RO(root);

            for (int i = 0; i < rootPtr->messageCount; ++i)
            {
                auto *msg = D_RO(rootPtr->messages[i]);
                if (msg->key_size != sizeof(KeyType))
                    continue;

                KeyType k;
                std::memcpy(&k, msg->key_data, sizeof(KeyType));
                if (k < low || k > high)
                    continue;

                Operations op = static_cast<Operations>(msg->opCode);
                ValueType v;
                std::memcpy(&v, msg->val_data, sizeof(ValueType));

                if (op == Operations::Insert || op == Operations::Update)
                {
                    auto it = std::find_if(result.begin(), result.end(),
                                           [&](const auto &pair)
                                           { return pair.first == k; });
                    if (it != result.end())
                    {
                        it->second = v; // update existing
                    }
                    else
                    {
                        result.emplace_back(k, v); // new entry
                    }
                }
                else if (op == Operations::Delete)
                {
                    result.erase(std::remove_if(result.begin(), result.end(),
                                                [&](const auto &pair)
                                                { return pair.first == k; }),
                                 result.end());
                }
            }
        }
        // 3. Sort and deduplicate
        std::sort(result.begin(), result.end(),
                  [](const auto &a, const auto &b)
                  { return a.first < b.first; });
        result.erase(std::unique(result.begin(), result.end(),
                                 [](const auto &a, const auto &b)
                                 { return a.first == b.first; }),
                     result.end());

        return result;
    }

    TOID(PersistentNode) findTargetNodeForKey(TOID(PersistentNode) node, const KeyType &key)
    {
        if (TOID_IS_NULL(node))
            return TOID_NULL(PersistentNode);

        auto *ptr = D_RO(node);
        if (!ptr)
        {
            std::cerr << "[ERROR] findTargetNodeForKey(): null pointer from D_RO().\n";
            return TOID_NULL(PersistentNode);
        }

        if (ptr->isLeaf)
            return node;

        int i = 0;
        while (i < ptr->keyCount && key >= ptr->keys[i])
            ++i;

        if (i > MAX_KEYS_PER_NODE || ptr->children[i] == 0)
        {
            std::cerr << "[ERROR] findTargetNodeForKey(): Invalid child index " << i << " for key " << key << "\n";
            return TOID_NULL(PersistentNode);
        }

        TOID(PersistentNode) child;
        child.oid.off = ptr->children[i];
        child.oid.pool_uuid_lo = poolUUID;

        return findTargetNodeForKey(child, key); // recurse
    }

    ErrorCode insert(KeyType key, ValueType value)
    {
        if (isReplaying)
            return ErrorCode::Success;

        logOperationBinary(Operations::Insert, key, value);

        // Tree is empty -> create root SSD leaf
        if (TOID_IS_NULL(persistentRoot))
        {
            TOID(PersistentNode) dummy;
            dummy.oid.off = 10000 + nextLeafFileID++;
            dummy.oid.pool_uuid_lo = poolUUID;

            std::string path = allocateLeafOnDisk(getPlatformPath("leaf_storage"), dummy.oid.off - 10000);
            if (path.empty())
                return ErrorCode::Error;

            persistentRoot = dummy;
            TOID(PMEMRoot)
            pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(pmemRoot)->persistentRoot = persistentRoot;
            pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));
        }

        // SSD leaf root
        if (isSSDLeaf(persistentRoot.oid.off))
        {
            std::string path = getLeafFilePathFromID(persistentRoot.oid.off);
            auto leaf = std::make_unique<SerializedLeafNode>();

            // SSD leaf may no longer exist after split
            if (!std::filesystem::exists(path))
            {
                // std::cerr << "[INSERT] SSD root was promoted -> switching to internal insert\n";
                goto buffered_insert;
            }

            if (!loadLeafFromDisk(path, *leaf))
            {
                std::cerr << "[INSERT] Failed to load SSD root leaf.\n";
                return ErrorCode::Error;
            }

            // Insert in sorted order
            int pos = 0;
            while (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] < key)
                ++pos;

            if (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] == key)
            {
                leaf->values[pos] = value;
            }
            else
            {
                for (int i = static_cast<int>(leaf->keyCount); i > pos; --i)
                {
                    leaf->keys[i] = leaf->keys[i - 1];
                    leaf->values[i] = leaf->values[i - 1];
                }
                leaf->keys[pos] = key;
                leaf->values[pos] = value;
                leaf->keyCount++;
            }

            // SSD leaf full -> split
            if (leaf->keyCount >= MAX_KEYS_PER_NODE)
            {
                // std::cout << "[ROOT SPLIT - SSD] Splitting SSD root leaf with "
                //           << leaf->keyCount << " keys at offset " << persistentRoot.oid.off << "\n";

                uint64_t oldSSDId = persistentRoot.oid.off;

                TOID(PersistentNode) newRoot = allocatePersistentNode(false);
                if (TOID_IS_NULL(newRoot))
                    return ErrorCode::Error;
                D_RW(newRoot)->children[0] = oldSSDId;
                D_RW(newRoot)->keyCount = 0;

                persistentRoot = newRoot;
                TOID(PMEMRoot)
                pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                D_RW(pmemRoot)->persistentRoot = persistentRoot;
                pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));

                if (!saveLeafToDisk(path, *leaf))
                {
                    std::cerr << "[INSERT] Failed to persist SSD leaf before split.\n";
                    return ErrorCode::Error;
                }

                if (splitSSDLeaf(persistentRoot, oldSSDId, key) != ErrorCode::Success)
                    return ErrorCode::Error;

                // std::cout << "[ROOT SPLIT - SSD] New internal root created with offset "
                //           << persistentRoot.oid.off << "\n";

                // Re-fetch updated root
                TOID(PMEMRoot)
                root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                persistentRoot = D_RW(root)->persistentRoot;
                // std::cout << "[INSERT] Re-fetched persistentRoot after split: offset = " << persistentRoot.oid.off << "\n";

                // Optimization: If the key is already in SSD and equals the intended value, skip buffering
                for (int i = 0; i < static_cast<int>(leaf->keyCount); ++i)
                {
                    if (leaf->keys[i] == key && leaf->values[i] == value)
                    {
                        // std::cout << "[INSERT] Optimization: Key " << key << " already persisted -> skipping buffer\n";
                        maybeCheckpoint();
                        return ErrorCode::Success;
                    }
                }

                goto buffered_insert;
            }

            // Save updated SSD leaf
            if (!saveLeafToDisk(path, *leaf))
            {
                std::cerr << "[INSERT] Failed to save SSD root leaf.\n";
                return ErrorCode::Error;
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        // Internal node path -> use shared buffer
    buffered_insert:
        // Re-fetch root after potential split (ensures correctness)
        TOID(PMEMRoot)
        pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        persistentRoot = D_RW(pmemRoot)->persistentRoot;

        if (isSSDLeaf(persistentRoot.oid.off))
        {
            std::cerr << "[INSERT] ERROR: Still on SSD root after split? Not buffering.\n";
            return ErrorCode::Error;
        }

        // Find the correct internal node to buffer into
        TOID(PersistentNode) bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget))
        {
            std::cerr << "[INSERT] ERROR: Could not find internal node to buffer into.\n";
            return ErrorCode::Error;
        }

        // Buffer the insert
        ErrorCode res = insertToNVMSharedBuffer(Operations::Insert, key, value);
        if (res != ErrorCode::Success)
            return res;

        maybeCheckpoint();

        return ErrorCode::Success;
    }

    TOID(PersistentNode) findTargetInternalNodeForKey(TOID(PersistentNode) node, KeyType key)
    {
        if (TOID_IS_NULL(node))
            return TOID_NULL(PersistentNode);

        // Protection: This function should never be called on SSD leaf nodes
        if (isSSDLeaf(node.oid.off))
        {
            std::cerr << "[BUG] Called findTargetInternalNodeForKey() on SSD leaf node " << node.oid.off << "\n";
            return TOID_NULL(PersistentNode);
        }

        auto *ptr = D_RO(node);
        if (!ptr)
        {
            std::cerr << "[ERROR] D_RO failed on node.\n";
            return TOID_NULL(PersistentNode);
        }

        // Protection: Structural validation
        if (ptr->keyCount == 0 || ptr->children[0] == 0)
        {
            std::cerr << "[FATAL] Internal node at offset " << node.oid.off
                      << " is structurally invalid (keyCount=0 or missing child).\n";
            return node;
        }

        if (ptr->isLeaf)
        {
            std::cerr << "[FATAL] findTargetInternalNodeForKey() reached a leaf at offset "
                      << node.oid.off << " — this should never happen.\n";
            return TOID_NULL(PersistentNode);
        }

        // Binary search or linear search for child index
        int i = 0;
        while (i < ptr->keyCount && key >= ptr->keys[i])
            ++i;

        if (i < 0 || i > ptr->keyCount || ptr->children[i] == 0)
        {
            std::cerr << "[ERROR] Invalid child index or null pointer: i=" << i
                      << ", keyCount=" << ptr->keyCount
                      << ", node.offset=" << node.oid.off
                      << " for this key " << key << "\n";
            return TOID_NULL(PersistentNode);
        }

        TOID(PersistentNode) child;
        child.oid.off = ptr->children[i];
        child.oid.pool_uuid_lo = poolUUID;

        // STOP if child is an SSD leaf
        if (isSSDLeaf(child.oid.off))
        {
            std::cout << "[ROUTING] Target child is SSD leaf -> stop at internal node " << node.oid.off << "\n";
            return node;
        }

        auto *childPtr = D_RO(child);
        if (!childPtr)
        {
            std::cerr << "[ERROR] D_RO(child) failed at offset " << child.oid.off << "\n";
            return TOID_NULL(PersistentNode);
        }

        if (childPtr->isLeaf)
        {
            return node;
        }

        return findTargetInternalNodeForKey(child, key);
    }

    // UPSERT OP (Insert if absent, else Update)
    ErrorCode upsert(KeyType key, ValueType value)
    {
        logOperationBinary(Operations::Upsert, key, value);

        if (TOID_IS_NULL(persistentRoot))
        {
            persistentRoot = allocatePersistentNode(true);
        }

        message msg;
        msg.opCode = static_cast<uint8_t>(Operations::Upsert);
        msg.key_size = sizeof(KeyType);
        msg.val_size = sizeof(ValueType);
        std::memcpy(msg.key_data, &key, sizeof(KeyType));
        std::memcpy(msg.val_data, &value, sizeof(ValueType));

        ErrorCode res = appendMessageToPersistentNode(persistentRoot, msg);
        if (res != ErrorCode::Success)
            return res;

        auto *rootPtr = D_RW(persistentRoot);
        if (rootPtr->keyCount >= MAX_KEYS_PER_NODE)
        {
            // std::cout << "[SPLIT] Root full — triggering recursive split.\n";
            splitPersistentNode(TOID_NULL(PersistentNode), persistentRoot);
        }

        maybeCheckpoint();
        return ErrorCode::Success;
    }

    void printNVMMessageToNodeMap() const
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RO(auxRoot);

        // std::cout << "\n--- [DEBUG] Persistent messageToNodeMap ---\n";
        if (auxPtr->messageToNodeMapCount == 0)
        {
            std::cout << "(empty)\n";
            return;
        }

        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i)
        {
            auto *entry = D_RO(auxPtr->messageToNodeMap[i]);
            // std::cout << "Key[" << entry->key << "] => " << nodeKeySummary(entry->node_id) << "\n";
        }
    }

    void printReadCache() const
    {
        std::cout << "\n--- [DEBUG] DRAM Read Cache ---\n";
        if (readCache.empty())
        {
            std::cout << "(empty)\n";
            return;
        }
        for (auto &[k, v] : readCache)
        {
            std::cout << "Key: " << k << ", Value: " << v << "\n";
        }
    }

    void printNVMSharedBuffer()
    {
        TOID(PMEMRoot)
        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *rootPtr = D_RO(root);
        std::cout << "\n[NVM BUFFER DEBUG] Current entries: " << rootPtr->messageCount << "\n";
        for (int i = 0; i < rootPtr->messageCount; ++i)
        {
            auto *msg = D_RO(rootPtr->messages[i]);
            std::cout << decodeMessageEntry(i, *msg) << "\n";
        }
    }

    void printNVMNodeFrequency()
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RO(auxRoot);

        std::cout << "\n--- [DEBUG] Persistent Node Frequency ---\n";
        for (int i = 0; i < auxPtr->nodeFrequencyCount; ++i)
        {
            auto *entry = D_RO(auxPtr->nodeFrequencyMap[i]);
            std::cout << "NodeID[" << entry->node_id << "] => freq=" << entry->frequency << "\n";
        }
    }

    void printNVMNodeMessageMap() const
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RO(auxRoot);

        std::cout << "\n--- [DEBUG] Persistent nodeMessageMap ---\n";
        if (auxPtr->nodeMessageMapCount == 0)
        {
            std::cout << "(empty)\n";
            return;
        }

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i)
        {
            auto *entry = D_RO(auxPtr->nodeMessageMap[i]);
            std::cout << nodeKeySummary(entry->node_id) << " => buffered keys: ";

            for (int j = 0; j < entry->key_list.count; ++j)
            {
                std::cout << entry->key_list.keys[j] << " ";
            }
            std::cout << "\n";
        }
    }

    std::string decodeMessageEntry(int index, const message &msg)
    {
        std::stringstream ss;
        ss << "[" << index << "] ";

        switch (msg.opCode)
        {
        case 0:
            ss << "Insert";
            break;
        case 1:
            ss << "Search";
            break;
        case 2:
            ss << "Update";
            break;
        case 3:
            ss << "Delete";
            break;
        case 4:
            ss << "Upsert";
            break;
        default:
            ss << "Unknown(" << static_cast<int>(msg.opCode) << ")";
        }

        ss << " | Key: ";
        if (msg.key_size == 4)
        {
            int32_t key;
            std::memcpy(&key, msg.key_data, 4);
            ss << key;
        }
        else if (msg.key_size == 8)
        {
            int64_t key;
            std::memcpy(&key, msg.key_data, 8);
            ss << key;
        }
        else if (msg.key_size == 16)
        {
            for (int i = 0; i < 16; ++i)
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(msg.key_data[i]));
        }
        else
        {
            ss << "(unknown/" << msg.key_size << " bytes)";
        }

        ss << " | Value: ";
        if (msg.val_size == 4)
        {
            int32_t val;
            std::memcpy(&val, msg.val_data, 4);
            ss << val;
        }
        else if (msg.val_size == 8)
        {
            int64_t val;
            std::memcpy(&val, msg.val_data, 8);
            ss << val;
        }
        else if (msg.val_size == 16)
        {
            for (int i = 0; i < 16; ++i)
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(msg.val_data[i]));
        }
        else
        {
            ss << "(unknown/" << msg.val_size << " bytes)";
        }

        return ss.str();
    }

    std::optional<std::tuple<Operations, ValueType>> lookupInNVMBuffer(const KeyType &key)
    {
        if (!pmemPoolHandle)
            return std::nullopt;

        TOID(PMEMRoot)
        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);

        auto *rootPtr = D_RO(root);

        for (int i = 0; i < rootPtr->messageCount; ++i)
        {
            auto *msg = D_RO(rootPtr->messages[i]);
            if (msg->key_size != sizeof(KeyType))
                continue;

            if (std::memcmp(msg->key_data, &key, sizeof(KeyType)) == 0)
            {
                Operations op = static_cast<Operations>(msg->opCode);
                ValueType val;
                std::memcpy(&val, msg->val_data, sizeof(ValueType));
                return std::make_tuple(op, val);
            }
        }

        return std::nullopt; // Not found
    }

    ErrorCode flushMostBufferedNode()
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        if (auxPtr->nodeMessageMapCount == 0)
            return ErrorCode::Success;

        // Step 1: Find node with most buffered keys
        int maxIndex = -1;
        int maxKeys = -1;
        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i)
        {
            auto *entry = D_RO(auxPtr->nodeMessageMap[i]);
            if (entry->key_list.count > maxKeys)
            {
                maxKeys = entry->key_list.count;
                maxIndex = i;
            }
        }

        if (maxIndex == -1)
        {
            std::cerr << "[FLUSH] No node with buffered messages.\n";
            return ErrorCode::Error;
        }

        auto *entry = D_RO(auxPtr->nodeMessageMap[maxIndex]);
        uint64_t node_id = entry->node_id;

        auto bufferedKeys = getBufferedKeysForNode(node_id);

        // std::cout << "[FLUSH NODE] Flushing node_id = " << node_id << "\n";
        // std::cout << "[FLUSH NODE] Buffered keys in this node: ";
        // for (auto k : bufferedKeys) std::cout << k << " ";
        // std::cout << "\n";

        for (const auto &key : bufferedKeys)
        {
            // std::cerr << "[FLUSH FLUSHING KEY " << key << "\n";
            auto messageOpt = lookupInNVMBuffer(key);
            if (!messageOpt.has_value())
                continue;

            auto [op, val] = messageOpt.value();

            // Recompute buffering target after splits
            TOID(PersistentNode) flushParent = findTargetInternalNodeForKey(persistentRoot, key);
            if (TOID_IS_NULL(flushParent))
            {
                std::cerr << "[FLUSH] Could not find internal node for key " << key << "\n";
                continue;
            }

            uint64_t correct_id = getNodeIDFromPersistentNode(flushParent);

            if (correct_id != node_id)
            {
                std::cout << "[FLUSH] Reassigning key " << key
                          << " from stale node " << node_id
                          << " -> correct node " << correct_id << "\n";

                if (correct_id == persistentRoot.oid.off)
                {
                    std::cout << "[FLUSH] Key " << key << " now buffered at new root.\n";
                }

                // Ensure clean reassignment
                removeKeyFromNodeMessageMap(node_id, key);
                removeKeyFromMessageToNodeMap(key);

                addKeyToNodeMessageMap(correct_id, key);
                insertToMessageToNodeMap(key, correct_id);

                continue;
            }

            auto *parentPtr = D_RW(flushParent);
            int i = 0;
            while (i < parentPtr->keyCount && key >= parentPtr->keys[i])
                ++i;

            if (i > MAX_KEYS_PER_NODE || parentPtr->children[i] == 0)
            {
                std::cerr << "[FLUSH] Invalid child index for key " << key << ". Skipping.\n";
                continue;
            }

            TOID(PersistentNode) child = findLeafNodeForKey(flushParent, key);
            if (TOID_IS_NULL(child) || !isSSDLeaf(child.oid.off))
            {
                std::cerr << "[FLUSH] ERROR: Target node " << child.oid.off
                          << " is not a valid SSD leaf for key " << key << ". Trying to reassign...\n";

                // Step 1: Find internal node that *routes to* correct SSD leaf
                TOID(PersistentNode) correctParent = findTargetInternalNodeForKey(persistentRoot, key);
                if (!TOID_IS_NULL(correctParent))
                {
                    uint64_t new_id = getNodeIDFromPersistentNode(correctParent);
                    removeKeyFromAllNodeMessageMaps(key);
                    removeKeyFromMessageToNodeMap(key);
                    addKeyToNodeMessageMap(new_id, key);
                    insertToMessageToNodeMap(key, new_id);
                    std::cout << "[REMAP INTERNAL] Key " << key << " reassigned to correct internal node " << new_id << "\n";
                }
                else
                {
                    std::cerr << "[REMAP FAILED] Could not find internal node for key " << key << "\n";
                }

                continue; // retry next round
            }

            bool isSSD = isSSDLeaf(child.oid.off);
            auto leaf = std::make_unique<SerializedLeafNode>();
            auto *childPtr = isSSD ? nullptr : D_RW(child);

            if (isSSD)
            {
                // std::cerr << "[FLUSH oad SSD leaf: " << child.oid.off << "\n";
                std::string filePath = getLeafFilePathFromID(child.oid.off);
                // std::cerr << "[FLUSH oad SSD leaf: " << filePath << "\n";
                if (!loadLeafFromDisk(filePath, *leaf))
                {
                    std::cerr << "[FLUSH] Failed to load SSD leaf: " << filePath << "\n";
                    continue;
                }
            }
            else
            {
                if (!childPtr)
                    continue;
                std::memset(&leaf, 0, sizeof(SerializedLeafNode));
                leaf->isLeaf = 1;
                leaf->keyCount = childPtr->keyCount;
                std::memcpy(leaf->keys, childPtr->keys, sizeof(childPtr->keys));
                std::memcpy(leaf->values, childPtr->children, sizeof(childPtr->children));
            }
            // std::cout << "[DEBUG] loadLeafFromDisk -> keyCount = " << leaf->keyCount << "\n";

            int pos = 0;
            while (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] < key)
                ++pos;

            if (op == Operations::Insert || op == Operations::Update)
            {
                // std::cout << "[FLUSH DEBUG] SSD Leaf " << child.oid.off
                //<< " BEFORE insert: keyCount = " << leaf->keyCount
                // << ", inserting key = " << key << "\n";

                if (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] == key)
                {
                    leaf->values[pos] = val;
                }
                else
                {
                    for (int j = static_cast<int>(leaf->keyCount); j > pos; --j)
                    {
                        leaf->keys[j] = leaf->keys[j - 1];
                        leaf->values[j] = leaf->values[j - 1];
                    }
                    leaf->keys[pos] = key;
                    leaf->values[pos] = val;
                    leaf->keyCount++;
                    // std::cout << "[FLUSH DEBUG] SSD Leaf " << child.oid.off
                    //<< " AFTER insert: keyCount = " << leaf->keyCount << "\n";
                }

                // std::cout << "[FLUSH CHECK] Leaf keyCount = " << leaf->keyCount << ", key = " << key << "\n";

                if (isSSD && leaf->keyCount >= MAX_KEYS_PER_NODE)
                {
                    // std::cout << "[FLUSH DEBUG] Initiating SSD split for leaf: " << child.oid.off
                    //<< ", parent: " << flushParent.oid.off << "\n";
                    if (!TOID_IS_NULL(flushParent))
                    {
                        auto *p = D_RW(flushParent);
                        // std::cout << "[FLUSH DEBUG] Parent keyCount = " << p->keyCount << "\n";
                    }
                    else
                    {
                        std::cerr << "[FLUSH ERROR] flushParent is NULL\n";
                    }
                    // Fallback: re-locate parent in case flushParent is stale

                    TOID(PersistentNode) revalidatedParent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), child);
                    if (!TOID_IS_NULL(revalidatedParent))
                    {
                        flushParent = revalidatedParent;
                    }

                    std::string path = getLeafFilePathFromID(child.oid.off);

                    if (!saveLeafToDisk(path, *leaf))
                    {
                        std::cerr << "[FLUSH] Failed to persist SSD leaf before split.\n";
                        return ErrorCode::Error;
                    }
                    if (!TOID_IS_NULL(flushParent))
                    {
                        auto *p = D_RW(flushParent);
                        if (flushParent.oid.off == persistentRoot.oid.off && p->keyCount >= MAX_KEYS_PER_NODE)
                        {
                            // std::cout << "[FLUSH] Root is full -> Pre-splitting root before continuing SSD split\n";
                            TOID(PersistentNode) grandparent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), flushParent);
                            ErrorCode res = splitPersistentNode(grandparent, flushParent);
                            if (res != ErrorCode::Success)
                            {
                                std::cerr << "[FLUSH] Root split failed.\n";
                                return res;
                            }

                            // Refresh root + flushParent after split
                            TOID(PMEMRoot)
                            pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                            persistentRoot = D_RW(pmemRoot)->persistentRoot;
                            flushParent = findTargetInternalNodeForKey(persistentRoot, key);
                            if (TOID_IS_NULL(flushParent))
                            {
                                std::cerr << "[FLUSH] Could not re-fetch parent after root split.\n";
                                return ErrorCode::Error;
                            }
                        }
                    }

                    // Now trigger the split (splitSSDLeaf will reload from disk)
                    ErrorCode splitRes = splitSSDLeaf(flushParent, child.oid.off, key);

                    if (splitRes != ErrorCode::Success)
                    {
                        std::cerr << "[FLUSH] Failed to split SSD leaf.\n";
                        return splitRes;
                    }
                    // std::cout << "[FLUSH] SSD split complete — removing key " << key << " from buffer to prevent re-split.\n";
                    removeKeyFromAllNodeMessageMaps(key);
                    removeKeyFromMessageToNodeMap(key);
                    removeFromNVMBuffer(key);
                    continue;
                }
            }
            else if (op == Operations::Delete)
            {
                std::cout << "[DELETE] Processing delete for key: " << key << "\n";

                if (pos < static_cast<int>(leaf->keyCount) && leaf->keys[pos] == key)
                {
                    std::cout << "[DELETE] Key " << key << " found in leaf. Removing...\n";
                    for (int j = pos; j < static_cast<int>(leaf->keyCount) - 1; ++j)
                    {
                        leaf->keys[j] = leaf->keys[j + 1];
                        leaf->values[j] = leaf->values[j + 1];
                    }
                    leaf->keyCount--;

                    // REMOVE FROM INTERNAL NODE IF FOUND
                    TOID(PersistentNode) parent = flushParent;
                    for (int j = 0; j < parentPtr->keyCount; ++j)
                    {
                        if (parentPtr->keys[j] == key)
                        {
                            std::cout << "[DELETE] Key " << key << " also exists in parent. Removing from internal node.\n";
                            for (int k = j; k < parentPtr->keyCount - 1; ++k)
                                parentPtr->keys[k] = parentPtr->keys[k + 1];
                            for (int k = j + 1; k <= parentPtr->keyCount; ++k)
                                parentPtr->children[k] = parentPtr->children[k + 1];
                            parentPtr->keyCount--;

                            std::cout << "[DELETE] COUNT " << parentPtr->keyCount << " NUMBERS\n";
                            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
                            // After internal key removal, check for underflow
                            if (parentPtr->keyCount < (m_nDegree / 2))
                            {
                                if (parent.oid.off == persistentRoot.oid.off)
                                {
                                    // ROOT UNDERFLOW: shrink if single child
                                    std::cout << "[DELETE] Internal root underflowed. Shrinking root...\n";

                                    TOID(PersistentNode) newRoot;
                                    newRoot.oid.off = parentPtr->children[0];
                                    newRoot.oid.pool_uuid_lo = poolUUID;

                                    pmemobj_free(&persistentRoot.oid);
                                    persistentRoot = newRoot;

                                    TOID(PMEMRoot)
                                    pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                                    D_RW(pmemRoot)->persistentRoot = persistentRoot;
                                    pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));
                                }
                                else
                                {
                                    // NON-ROOT UNDERFLOW -> recurse
                                    std::cout << "[DELETE] Internal parent " << parent.oid.off
                                              << " underflowed after deleting key " << key << "\n";

                                    TOID(PersistentNode) grandparent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), parent);
                                    if (!TOID_IS_NULL(grandparent))
                                    {
                                        ErrorCode res = handleUnderflowPersistent(grandparent, parent);
                                        if (res != ErrorCode::Success)
                                        {
                                            std::cerr << "[DELETE] Failed to handle underflow in internal parent " << parent.oid.off << "\n";
                                            return res;
                                        }
                                    }
                                }
                            }

                            break;
                        }
                    }

                    // UNDERFLOW CHECK
                    if (leaf->keyCount < MAX_KEYS_PER_NODE / 2)
                    {
                        std::cout << "[DELETE] Leaf underflow detected. Triggering merge/borrow...\n";
                        auto res = handleUnderflowPersistent(flushParent, child);
                        if (res != ErrorCode::Success)
                            std::cerr << "[DELETE] Underflow recovery failed for key " << key << "\n";
                    }
                }
                else
                {
                    std::cout << "[DELETE] Key " << key << " not found in leaf. No deletion performed.\n";
                }
            }

            if (isSSD)
            {
                std::string path = getLeafFilePathFromID(child.oid.off);
                if (!saveLeafToDisk(path, *leaf))
                {
                    std::cerr << "[FLUSH] Failed to save SSD leaf: " << path << "\n";
                    continue;
                }
            }
            else
            {
                childPtr->keyCount = leaf->keyCount;
                std::memcpy(childPtr->keys, leaf->keys, sizeof(childPtr->keys));
                std::memcpy(childPtr->children, leaf->values, sizeof(childPtr->children));
                pmemobj_persist(pmemPoolHandle, childPtr, sizeof(PersistentNode));
            }

            removeKeyFromAllNodeMessageMaps(key);
            removeKeyFromMessageToNodeMap(key);
            removeFromNVMBuffer(key);
        }

        return ErrorCode::Success;
    }

    TOID(PersistentNode) findLeafNodeForKey(TOID(PersistentNode) node, const KeyType &key)
    {
        if (TOID_IS_NULL(node))
            return TOID_NULL(PersistentNode);

        auto *ptr = D_RO(node);
        if (!ptr)
            return TOID_NULL(PersistentNode);

        if (ptr->isLeaf || isSSDLeaf(node.oid.off))
            return node;

        int i = 0;
        while (i < ptr->keyCount && key >= ptr->keys[i])
            ++i;

        if (i >= MAX_KEYS_PER_NODE + 1 || ptr->children[i] == 0)
            return TOID_NULL(PersistentNode);

        TOID(PersistentNode) child;
        child.oid.off = ptr->children[i];
        child.oid.pool_uuid_lo = poolUUID;

        return findLeafNodeForKey(child, key);
    }

    void removeFromNVMBuffer(const KeyType &key)
    {
        TOID(PMEMRoot)
        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *rootPtr = D_RW(root);

        for (int i = 0; i < rootPtr->messageCount; ++i)
        {
            auto *msg = D_RW(rootPtr->messages[i]);
            if (msg->key_size == sizeof(KeyType) &&
                std::memcmp(msg->key_data, &key, sizeof(KeyType)) == 0)
            {
                pmemobj_free(&rootPtr->messages[i].oid);
                for (int j = i + 1; j < rootPtr->messageCount; ++j)
                {
                    rootPtr->messages[j - 1] = rootPtr->messages[j];
                }
                rootPtr->messageCount--;
                pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));
                return;
            }
        }
    }

    TOID(PersistentNode) findNodeById(TOID(PersistentNode) node, uint64_t node_id) const
    {
        if (TOID_IS_NULL(node))
            return TOID_NULL(PersistentNode);

        if (node.oid.off == node_id)
        {
            return node;
        }

        auto *ptr = D_RO(node);
        if (!ptr || ptr->isLeaf)
            return TOID_NULL(PersistentNode);

        for (int i = 0; i <= ptr->keyCount; ++i)
        {
            TOID(PersistentNode) child;
            child.oid.off = ptr->children[i];
            child.oid.pool_uuid_lo = poolUUID;

            TOID(PersistentNode) found = findNodeById(child, node_id);
            if (!TOID_IS_NULL(found))
                return found;
        }
        return TOID_NULL(PersistentNode);
    }

    std::string getPlatformPath(const std::string &filename)
    {
#ifdef _WIN32
        return "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\" + filename;
#else
        return "/home/ademzarrouki/Desktop/Benchmark/" + filename;
#endif
    }

    // Persistent nodeMessageMap Helpers
    void addKeyToNodeMessageMap(uint64_t node_id, KeyType key)
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i)
        {
            auto *entry = D_RW(auxPtr->nodeMessageMap[i]);
            if (entry->node_id == node_id)
            {
                for (int j = 0; j < entry->key_list.count; ++j)
                {
                    if (entry->key_list.keys[j] == key)
                        return; // already present
                }
                if (entry->key_list.count < MAX_KEYS_PER_NODE)
                {
                    entry->key_list.keys[entry->key_list.count++] = key;
                    pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeMessageEntry));
                }
                return;
            }
        }

        // New entry
        if (auxPtr->nodeMessageMapCount < MAX_NODES)
        {
            TOID(NodeMessageEntry)
            newEntry;
            if (pmemobj_alloc(pmemPoolHandle, &newEntry.oid, sizeof(NodeMessageEntry), 0, nullptr, nullptr) == 0)
            {
                auto *newPtr = D_RW(newEntry);
                newPtr->node_id = node_id;
                newPtr->key_list.count = 1;
                newPtr->key_list.keys[0] = key;
                pmemobj_persist(pmemPoolHandle, newPtr, sizeof(NodeMessageEntry));

                auxPtr->nodeMessageMap[auxPtr->nodeMessageMapCount++] = newEntry;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot));
            }
        }
    }

    void removeKeyFromNodeMessageMap(uint64_t node_id, KeyType key)
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i)
        {
            auto *entry = D_RW(auxPtr->nodeMessageMap[i]);
            if (entry->node_id == node_id)
            {
                auto &list = entry->key_list;
                int j = 0;
                while (j < list.count)
                {
                    if (list.keys[j] == key)
                    {
                        // Shift remaining keys left
                        for (int k = j + 1; k < list.count; ++k)
                        {
                            list.keys[k - 1] = list.keys[k];
                        }
                        list.count--;
                        pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeMessageEntry));

                        // Remove the entire entry if now empty
                        if (list.count == 0)
                        {
                            pmemobj_free(&auxPtr->nodeMessageMap[i].oid);
                            for (int m = i + 1; m < auxPtr->nodeMessageMapCount; ++m)
                            {
                                auxPtr->nodeMessageMap[m - 1] = auxPtr->nodeMessageMap[m];
                            }
                            auxPtr->nodeMessageMapCount--;
                            pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot));
                        }

                        return;
                    }
                    ++j;
                }
                return;
            }
        }
    }

    void removeKeyFromAllNodeMessageMaps(KeyType key)
    {
        TOID(PMEMRoot)
        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *rootPtr = D_RW(root);

        for (int i = 0; i < rootPtr->nodeMessageMapCount; /* no increment here */)
        {
            auto *entry = D_RW(rootPtr->nodeMessageMap[i]);
            auto &list = entry->key_list;

            bool found = false;
            for (int j = 0; j < list.count; ++j)
            {
                if (list.keys[j] == key)
                {
                    // Shift keys left
                    for (int k = j + 1; k < list.count; ++k)
                    {
                        list.keys[k - 1] = list.keys[k];
                    }
                    list.count--;
                    pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeMessageEntry));
                    found = true;
                    break;
                }
            }

            // If entry is now empty -> delete the whole entry
            if (list.count == 0)
            {
                pmemobj_free(&rootPtr->nodeMessageMap[i].oid);
                for (int j = i + 1; j < rootPtr->nodeMessageMapCount; ++j)
                {
                    rootPtr->nodeMessageMap[j - 1] = rootPtr->nodeMessageMap[j];
                }
                rootPtr->nodeMessageMapCount--;
                pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));
                // Do NOT increment i, because we shifted everything
            }
            else
            {
                ++i; // Only move forward if we didn't erase
            }
        }
    }

    void moveBufferedKeyBetweenNodes(uint64_t from_id, uint64_t to_id, KeyType key)
    {
        removeKeyFromNodeMessageMap(from_id, key);
        // addKeyToNodeMessageMap(to_id, key);
    }

    void insertToMessageToNodeMap(uint64_t key, uint64_t node_id)
    {
        removeKeyFromMessageToNodeMap(key);
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        // Overwrite if key already exists
        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i)
        {
            auto *entry = D_RW(auxPtr->messageToNodeMap[i]);
            if (entry->key == key)
            {
                entry->node_id = node_id;
                pmemobj_persist(pmemPoolHandle, entry, sizeof(MessageToNodeEntry));
                return;
            }
        }

        // Insert new entry
        if (auxPtr->messageToNodeMapCount < MAX_MESSAGES)
        {
            TOID(MessageToNodeEntry)
            newEntry;
            if (pmemobj_alloc(pmemPoolHandle, &newEntry.oid, sizeof(MessageToNodeEntry), 0, nullptr, nullptr) == 0)
            {
                D_RW(newEntry)->key = key;
                D_RW(newEntry)->node_id = node_id;
                pmemobj_persist(pmemPoolHandle, D_RW(newEntry), sizeof(MessageToNodeEntry));
                auxPtr->messageToNodeMap[auxPtr->messageToNodeMapCount++] = newEntry;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot));
            }
        }
    }

    std::optional<uint64_t> lookupNodeIdForKey(uint64_t key)
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RO(auxRoot);

        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i)
        {
            auto *entry = D_RO(auxPtr->messageToNodeMap[i]);
            if (entry->key == key)
            {
                return entry->node_id;
            }
        }

        return std::nullopt;
    }

    std::vector<KeyType> getBufferedKeysForNode(uint64_t node_id)
    {
        std::vector<KeyType> result;
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RO(auxRoot);

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i)
        {
            auto *entry = D_RO(auxPtr->nodeMessageMap[i]);
            if (entry->node_id == node_id)
            {
                for (int j = 0; j < entry->key_list.count; ++j)
                {
                    result.push_back(static_cast<KeyType>(entry->key_list.keys[j]));
                }
                break;
            }
        }
        return result;
    }

    void removeKeyFromMessageToNodeMap(uint64_t key)
    {
        TOID(PMEMRoot)
        auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i)
        {
            auto *entry = D_RW(auxPtr->messageToNodeMap[i]);
            if (entry->key == key)
            {
                // Free and shift
                pmemobj_free(&auxPtr->messageToNodeMap[i].oid);
                for (int j = i + 1; j < auxPtr->messageToNodeMapCount; ++j)
                {
                    auxPtr->messageToNodeMap[j - 1] = auxPtr->messageToNodeMap[j];
                }
                auxPtr->messageToNodeMapCount--;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot));
                return;
            }
        }
    }

    void setChildPointer(PersistentNode *node, int index, TOID(PersistentNode) child)
    {
        if (!node || TOID_IS_NULL(child))
            return;

        if (index < 0 || index > MAX_KEYS_PER_NODE)
        {
            std::cerr << "[ERROR] setChildPointer(): index out of bounds: " << index << "\n";
            return;
        }

        node->children[index] = child.oid.off;
        pmemobj_persist(pmemPoolHandle, &node->children[index], sizeof(uint64_t));
    }

    inline std::string allocateLeafOnDisk(const std::string &basePath, int id)
    {
        ensureLeafStorageDir(basePath);

        std::string path = basePath + "/leaf_" + std::to_string(id) + ".bin";
        // std::cout << "[ALLOC] Creating file: " << path << "\n";

        auto *node = new SerializedLeafNode(); // allocated on heap

        std::ofstream file(path, std::ios::binary);
        if (!file.is_open())
        {
            std::cerr << "[ERROR] Failed to open file stream for writing: " << path << "\n";
            delete node;
            return "";
        }

        file.write(reinterpret_cast<const char *>(node), sizeof(SerializedLeafNode));
        file.close();
        delete node;

        // std::cout << "[ALLOC] SSD leaf node created at: " << path << "\n";
        return path;
    }

    ErrorCode splitSSDLeaf(TOID(PersistentNode) parent, uint64_t leafID, KeyType key)
    {
        // std::cout << "[FLUSH DEBUG] Initiating SSD split for leaf: " << leafID << ", parent: " << parent.oid.off << "\n";

        std::string leafPath = getLeafFilePathFromID(leafID);
        auto original = std::make_unique<SerializedLeafNode>();
        if (!loadLeafFromDisk(leafPath, *original))
        {
            std::cerr << "[SPLIT] Failed to load leaf from: " << leafPath << "\n";
            return ErrorCode::Error;
        }
        // std::cout << "[FLUSH DEBUG] Original keyCount = " << original->keyCount << "\n";

        if (original->keyCount < MAX_KEYS_PER_NODE)
        {
            // std::cout << "[SPLIT] SSD leaf not full, no need to split.\n";
            return ErrorCode::Success;
        }

        int mid = original->keyCount / 2;

        auto sibling = std::make_unique<SerializedLeafNode>();
        sibling->keyCount = original->keyCount - mid;
        sibling->isLeaf = 1;
        for (int i = 0; i < sibling->keyCount; ++i)
        {
            sibling->keys[i] = original->keys[mid + i];
            sibling->values[i] = original->values[mid + i];
        }

        original->keyCount = mid;

        // Create sibling file
        uint64_t newLeafID = 10000 + nextLeafFileID++;
        std::string siblingPath = allocateLeafOnDisk(getPlatformPath("leaf_storage"), newLeafID - 10000);
        if (siblingPath.empty())
            return ErrorCode::Error;

        // Persist changes
        if (!saveLeafToDisk(leafPath, *original) || !saveLeafToDisk(siblingPath, *sibling))
        {
            std::cerr << "[SPLIT] Failed to persist SSD leaves.\n";
            return ErrorCode::Error;
        }

        uint64_t pivot = sibling->keys[0];

        // Reassign buffered keys ≥ pivot to new sibling
        uint64_t from_id = leafID;
        uint64_t to_id = newLeafID;
        auto buffered = getBufferedKeysForNode(getNodeIDFromPersistentNode(parent));

        for (const auto &key : buffered)
        {
            if (key >= pivot)
            {
                moveBufferedKeyBetweenNodes(from_id, to_id, key);
                insertToMessageToNodeMap(key, to_id);
                // std::cout << "[REMAP SSD] Key " << key << " reassigned from " << from_id << " -> " << to_id << " after SSD split\n";
            }
        }

        if (TOID_IS_NULL(parent))
        {
            std::cerr << "[SPLIT] ERROR: parent is null in SSD split.\n";
            return ErrorCode::Error;
        }

        auto *parentPtr = D_RW(parent);
        // std::cout << "[FLUSH DEBUG] Parent keyCount = " << parentPtr->keyCount << "\n";

        // If parent is full, split it first (recursive)
        if (parentPtr->keyCount >= MAX_KEYS_PER_NODE)
        {
            std::cerr << "[FLUSH] Root is full -> Pre-splitting root before continuing SSD split\n";

            TOID(PersistentNode) grandparent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), parent);
            ErrorCode res = splitPersistentNode(grandparent, parent);
            if (res != ErrorCode::Success)
            {
                std::cerr << "[FLUSH] Root split failed.\n";
                return res;
            }

            // Refresh root + parent after split
            TOID(PMEMRoot)
            pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            persistentRoot = D_RW(pmemRoot)->persistentRoot;
            parent = findTargetInternalNodeForKey(persistentRoot, key); // REVALIDATE parent
            if (TOID_IS_NULL(parent))
            {
                std::cerr << "[FLUSH] Could not re-fetch parent after root split.\n";
                return ErrorCode::Error;
            }
            parentPtr = D_RW(parent); // Refresh pointer
        }

        int insertIdx = parentPtr->keyCount;
        while (insertIdx > 0 && parentPtr->keys[insertIdx - 1] > pivot)
        {
            if (insertIdx >= MAX_KEYS_PER_NODE)
            {
                std::cerr << "[SPLIT] Shift index out of bounds!\n";
                return ErrorCode::Error;
            }
            parentPtr->keys[insertIdx] = parentPtr->keys[insertIdx - 1];
            parentPtr->children[insertIdx + 1] = parentPtr->children[insertIdx];
            insertIdx--;
        }

        if (insertIdx + 1 >= MAX_KEYS_PER_NODE + 1)
        {
            std::cerr << "[SPLIT] Cannot assign to children[" << (insertIdx + 1) << "] -> out of bounds!\n";
            return ErrorCode::Error;
        }

        parentPtr->keys[insertIdx] = pivot;
        parentPtr->children[insertIdx + 1] = newLeafID;
        parentPtr->keyCount++;
        if (parentPtr->keyCount == 0 || parentPtr->children[0] == 0)
        {
            std::cerr << "[FATAL] splitSSDLeaf(): Parent node became invalid (empty or missing child).\n";
            return ErrorCode::Error;
        }
        pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
        // Update persistentRoot if parent is root
        if (parent.oid.off == persistentRoot.oid.off)
        {
            TOID(PMEMRoot)
            root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(root)->persistentRoot = parent;
            persistentRoot = parent;
            pmemobj_persist(pmemPoolHandle, D_RW(root), sizeof(PMEMRoot));
            // std::cout << "[SPLIT SSD LEAF] Updated persistentRoot to internal parent with offset " << parent.oid.off << "\n";
        }

        return ErrorCode::Success;
    }

    ErrorCode splitPersistentNode(TOID(PersistentNode) parent, TOID(PersistentNode) node)
    {
        auto *nodePtr = D_RW(node);
        if (!nodePtr || nodePtr->isLeaf)
        {
            std::cerr << "[SPLIT] Error: Not a valid internal node.\n";
            return ErrorCode::Error;
        }

        int mid = nodePtr->keyCount / 2;
        uint64_t pivotKey = nodePtr->keys[mid];

        TOID(PersistentNode) sibling = allocatePersistentNode(false);
        if (TOID_IS_NULL(sibling))
            return ErrorCode::Error;
        auto *siblingPtr = D_RW(sibling);

        siblingPtr->keyCount = nodePtr->keyCount - mid - 1;

        // Keys: move from mid+1 onward
        for (int i = 0; i < siblingPtr->keyCount; ++i)
        {
            siblingPtr->keys[i] = nodePtr->keys[mid + 1 + i];
        }

        // Children: move from mid+1 onward (total = keyCount - mid)
        for (int i = 0; i <= siblingPtr->keyCount; ++i)
        {
            siblingPtr->children[i] = nodePtr->children[mid + 1 + i];
        }

        nodePtr->keyCount = mid;
        for (int i = mid; i < MAX_KEYS_PER_NODE; ++i)
            nodePtr->keys[i] = 0;

        for (int i = mid + 1; i < MAX_KEYS_PER_NODE + 1; ++i)
            nodePtr->children[i] = 0;

        pmemobj_persist(pmemPoolHandle, nodePtr, sizeof(PersistentNode));
        pmemobj_persist(pmemPoolHandle, siblingPtr, sizeof(PersistentNode));

        // Move buffered messages
        uint64_t from_id = getNodeIDFromPersistentNode(node);
        uint64_t to_id = getNodeIDFromPersistentNode(sibling);
        auto movedKeys = getBufferedKeysForNode(from_id);
        for (const auto &k : movedKeys)
        {
            if (k >= pivotKey)
            {
                moveBufferedKeyBetweenNodes(from_id, to_id, k);
                insertToMessageToNodeMap(static_cast<uint64_t>(k), to_id);
                std::cout << "[DEBUG] Moved buffered key " << k << " -> sibling during internal split\n";
                // printNVMNodeMessageMap();
            }
        }

        // std::cout << "[ROOT SPLIT - INTERNAL] Splitting internal root node with pivot "
        //           << pivotKey << " (offset = " << node.oid.off << ")\n";

        // Promote to root if no parent
        if (TOID_IS_NULL(parent))
        {
            TOID(PersistentNode) newRoot = allocatePersistentNode(false);
            if (TOID_IS_NULL(newRoot))
                return ErrorCode::Error;
            auto *rootPtr = D_RW(newRoot);

            rootPtr->keys[0] = pivotKey;
            rootPtr->keyCount = 1;
            setChildPointer(rootPtr, 0, node);
            setChildPointer(rootPtr, 1, sibling);
            if (rootPtr->keyCount == 0 || rootPtr->children[0] == 0)
            {
                std::cerr << "[FATAL] splitPersistentNode(): New root is invalid (empty).\n";
                return ErrorCode::Error;
            }

            persistentRoot = newRoot;
            TOID(PMEMRoot)
            root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(root)->persistentRoot = persistentRoot;
            if (rootPtr->keyCount == 0 || rootPtr->children[0] == 0)
            {
                std::cerr << "[PATCH] Resetting invalid new root (keyCount=0 or children[0]=0)\n";
                for (int i = 0; i <= MAX_KEYS_PER_NODE; ++i)
                    rootPtr->children[i] = 0;
                for (int i = 0; i < MAX_KEYS_PER_NODE; ++i)
                    rootPtr->keys[i] = 0;
            }

            pmemobj_persist(pmemPoolHandle, D_RW(root), sizeof(PMEMRoot));

            if (rootPtr->keyCount == 0 || rootPtr->children[0] == 0)
            {
                std::cerr << "[PATCH] Resetting invalid new root (keyCount=0 or children[0]=0)\n";
                for (int i = 0; i <= MAX_KEYS_PER_NODE; ++i)
                    rootPtr->children[i] = 0;
                for (int i = 0; i < MAX_KEYS_PER_NODE; ++i)
                    rootPtr->keys[i] = 0;
            }

            std::cout << "[ROOT SPLIT - INTERNAL] New root created with children "
                      << node.oid.off << " and " << sibling.oid.off << "\n";
        }

        else
        {
            auto *parentPtr = D_RW(parent);
            int insertIdx = parentPtr->keyCount;
            while (insertIdx > 0 && parentPtr->keys[insertIdx - 1] > pivotKey)
            {
                parentPtr->keys[insertIdx] = parentPtr->keys[insertIdx - 1];
                parentPtr->children[insertIdx + 1] = parentPtr->children[insertIdx];
                --insertIdx;
            }

            parentPtr->keys[insertIdx] = pivotKey;
            setChildPointer(parentPtr, insertIdx + 1, sibling);
            parentPtr->keyCount++;

            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));

            // Recursively split if overfull
            if (parentPtr->keyCount >= MAX_KEYS_PER_NODE)
            {
                TOID(PersistentNode) grandparent = findPersistentParent(persistentRoot, parent, node);
                return splitPersistentNode(grandparent, parent);
            }
        }
        return ErrorCode::Success;
    }

    TOID(PersistentNode)
    findPersistentParent(TOID(PersistentNode) current, TOID(PersistentNode) target, TOID(PersistentNode) childToFind)
    {
        if (TOID_IS_NULL(current))
            return TOID_NULL(PersistentNode);
        auto *curPtr = D_RO(current);

        if (!curPtr || curPtr->isLeaf)
            return TOID_NULL(PersistentNode); // leaf can't be parent

        for (int i = 0; i <= curPtr->keyCount; ++i)
        {
            if (curPtr->children[i] == childToFind.oid.off)
                return current;
        }

        for (int i = 0; i <= curPtr->keyCount; ++i)
        {
            if (curPtr->children[i] == 0)
                continue;

            TOID(PersistentNode) child;
            child.oid.off = curPtr->children[i];
            child.oid.pool_uuid_lo = poolUUID;

            TOID(PersistentNode) result = findPersistentParent(child, target, childToFind);
            if (!TOID_IS_NULL(result))
                return result;
        }

        return TOID_NULL(PersistentNode);
    }

    uint64_t getNodeIDFromPersistentNode(TOID(PersistentNode) node)
    {
        return node.oid.off;
    }

    void rangeQueryRecursive(TOID(PersistentNode) node, KeyType low, KeyType high, std::vector<std::pair<KeyType, ValueType>> &result)
    {
        if (TOID_IS_NULL(node))
            return;

        auto *ptr = D_RO(node);
        if (!ptr)
            return;

        if (ptr->isLeaf)
        {
            for (int i = 0; i < ptr->keyCount; ++i)
            {
                KeyType key = static_cast<KeyType>(ptr->keys[i]);
                if (key >= low && key <= high)
                {
                    ValueType val = static_cast<ValueType>(ptr->children[i]);
                    result.emplace_back(key, val);
                }
            }
        }
        else
        {
            int i = 0;
            while (i < ptr->keyCount && low > ptr->keys[i])
                ++i;

            for (; i <= ptr->keyCount; ++i)
            {
                if (i < ptr->keyCount && ptr->keys[i] > high)
                    break;

                TOID(PersistentNode) child;
                child.oid.off = ptr->children[i];
                child.oid.pool_uuid_lo = poolUUID;

                rangeQueryRecursive(child, low, high, result);
            }
        }
    }

    void displayPersistentTreeWithLeaves()
    {
        std::cout << "\n--- Persistent Bε-tree (Compact View With Leaves) ---\n";
        displayNodeWithLeaves(persistentRoot, 0);
    }

    void displayNodeWithLeaves(TOID(PersistentNode) node, int depth)
    {
        if (TOID_IS_NULL(node))
            return;

        std::string indent(depth * 2, ' ');
        if (isSSDLeaf(node.oid.off))
        {
            // Load and display SSD leaf contents
            std::string path = getLeafFilePathFromID(node.oid.off);
            SerializedLeafNode leaf;
            if (!loadLeafFromDisk(path, leaf))
            {
                std::cout << indent << "[Leaf " << node.oid.off << " FAILED TO LOAD]\n";
                return;
            }

            std::cout << indent << "[";
            for (int i = 0; i < static_cast<int>(leaf.keyCount); ++i)
            {
                std::cout << "(" << leaf.keys[i] << " -> " << leaf.values[i] << ") ";
            }
            std::cout << "]\n";
            return;
        }

        auto *ptr = D_RW(node);

        // Print internal node keys
        std::cout << indent << "[";
        for (int i = 0; i < ptr->keyCount; ++i)
            std::cout << ptr->keys[i] << " ";
        std::cout << "]\n";

        // Recursively print children
        for (int i = 0; i <= ptr->keyCount; ++i)
        {
            TOID(PersistentNode) child;
            child.oid.off = ptr->children[i];
            child.oid.pool_uuid_lo = poolUUID;
            displayNodeWithLeaves(child, depth + 1);
        }
    }

    std::string nodeKeySummary(uint64_t node_id) const
    {
        TOID(PersistentNode) node = findNodeById(persistentRoot, node_id);
        if (TOID_IS_NULL(node))
            return "NodeID[" + std::to_string(node_id) + "] - [not found]";

        auto *ptr = D_RO(node);
        std::stringstream ss;
        ss << "[";
        for (int i = 0; i < ptr->keyCount; ++i)
            ss << ptr->keys[i] << (i < ptr->keyCount - 1 ? " " : "");
        ss << "]";
        return ss.str();
    }

public:
    void printSSDLeaf(uint64_t nodeId)
    {
        std::string path = getLeafFilePathFromID(nodeId);
        auto leaf = std::make_unique<SerializedLeafNode>();
        if (!loadLeafFromDisk(path, *leaf))
        {
            std::cerr << "[DEBUG] Failed to load SSD leaf " << nodeId << "\n";
            return;
        }

        std::cout << "[SSD Leaf " << nodeId << "] Keys: ";
        for (size_t i = 0; i < leaf->keyCount; ++i)
        {
            std::cout << "(" << leaf->keys[i] << " -> " << leaf->values[i] << ") ";
        }
        std::cout << "\n";
    }
};
