#pragma once
#include <libpmemobj.h>
#include <libpmemobj/base.h>
#include <libpmemobj/pool_base.h>
#include <libpmemobj/atomic_base.h>
#include "Node.hpp"
#include "ErrorCodes.h"
#include "Operations.h"
#include "nvm.hpp"
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
    TOID(PersistentNode)
    persistentRoot;
    PMEMobjpool *pmemPoolHandle = nullptr; // NVM pool

    uint32_t m_nDegree;
    std::string logFilename = getPlatformPath("nvm_tree_test1.log");
    int opCounter = 0;
    int checkpointFrequency = 5;
    bool isReplaying = false;
    uint64_t poolUUID = 0;

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
        TOID(PMEMRoot) pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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

        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        PMEMoid rootOID = pmemobj_oid(D_RW(root));
        this->poolUUID = rootOID.pool_uuid_lo;
    }

    TOID(PersistentNode) allocatePersistentNode(bool isLeaf)
    {
        TOID(PersistentNode)
        node;
        int ret = pmemobj_alloc(pmemPoolHandle, &node.oid, sizeof(PersistentNode), 0, nullptr, nullptr);

        if (ret != 0 || TOID_IS_NULL(node))
        {
            std::cerr << "[ERROR] Failed to allocate PersistentNode in PMEM (ret=" << ret << ").\n";
            return TOID_NULL(PersistentNode);
        }

        auto *ptr = D_RW(node);
        ptr->isLeaf = isLeaf ? 1 : 0;
        ptr->keyCount = 0;
        ptr->buffer_offset = 0;
        std::memset(ptr->keys, 0, sizeof(ptr->keys));
        std::memset(ptr->children, 0, sizeof(ptr->children));
        std::memset(ptr->buffer, 0, NODE_BUFFER_SIZE);

        pmemobj_persist(pmemPoolHandle, ptr, sizeof(PersistentNode));
        return node;
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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

        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
                // Remove message
                for (int j = existingIndex + 1; j < rootPtr->messageCount; ++j)
                    rootPtr->messages[j - 1] = rootPtr->messages[j];
                rootPtr->messageCount--;
                pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));

                // Clean metadata
                auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
                if (nodeIdOpt.has_value())
                {
                    removeKeyFromAllNodeMessageMaps(key);
                }
                removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));

                return ErrorCode::Success;
            }

            // Regular overwrite (Insert/Update)
            msgPtr->opCode = static_cast<uint8_t>(op);
            msgPtr->key_size = sizeof(KeyType);
            msgPtr->val_size = (op == Operations::Delete) ? 0 : sizeof(ValueType);
            std::memcpy(msgPtr->key_data, &key, sizeof(KeyType));
            if (op != Operations::Delete)
            {
                std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
            }
            pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
            return ErrorCode::Success;
        }

        // HANDLE INSERTION OF NEW DELETE
        if (op == Operations::Delete)
        {
            // Message does not exist, but key might still be in maps -> clean stale metadata
            auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
            if (nodeIdOpt.has_value())
            {
                removeKeyFromAllNodeMessageMaps(key);
            }
            removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));
            //return ErrorCode::Success; // nothing more to store
        }

        // FLUSH IF FULL
        while (rootPtr->messageCount >= MAX_NVM_MESSAGES)
        {
            auto flushResult = flushMostBufferedNode();
            if (flushResult != ErrorCode::Success)
            {
                std::cerr << "[ERROR] Failed to flush any node. Shared buffer stuck.\n";
                return ErrorCode::Error;
            }

            root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            rootPtr = D_RW(root);
        }

        auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
        if (nodeIdOpt.has_value())
        {
            removeKeyFromAllNodeMessageMaps(key);
        }
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
    ErrorCode remove(KeyType key) {
        if (isReplaying) return ErrorCode::Success;
    
        logOperationBinary(Operations::Delete, key);
    
        if (TOID_IS_NULL(persistentRoot)) {
            std::cerr << "[REMOVE] No persistent root exists.\n";
            return ErrorCode::KeyDoesNotExist;
        }
    
        auto* rootPtr = D_RW(persistentRoot);
        if (rootPtr->isLeaf) {
            for (int i = 0; i < rootPtr->keyCount; ++i) {
                if (rootPtr->keys[i] == key) {
                    for (int j = i; j < rootPtr->keyCount - 1; ++j) {
                        rootPtr->keys[j] = rootPtr->keys[j + 1];
                        rootPtr->children[j] = rootPtr->children[j + 1];
                    }
                    rootPtr->keyCount--;
                    pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PersistentNode));
                    return ErrorCode::Success;
                }
            }
            return ErrorCode::KeyDoesNotExist;
        }
    
        // Else: route delete to buffer
        TOID(PersistentNode) bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget)) {
            std::cerr << "[REMOVE] Could not find internal node for key " << key << "\n";
            return ErrorCode::Error;
        }
    
        // Push delete to NVM buffer
        ErrorCode res = insertToNVMSharedBuffer(Operations::Delete, key);
        if (res != ErrorCode::Success) return res;
    
        // Update tracking maps
        uint64_t node_id = getNodeIDFromPersistentNode(bufferTarget);
        addKeyToNodeMessageMap(node_id, key);
        insertToMessageToNodeMap(static_cast<uint64_t>(key), node_id);
        incrementNodeFrequency(node_id);
        maybeCheckpoint();
        if (!TOID_IS_NULL(persistentRoot)) {
            auto* rootPtr = D_RW(persistentRoot);
            if (!rootPtr->isLeaf && rootPtr->keyCount == 0) {
                TOID(PersistentNode) newRoot;
                newRoot.oid.off = rootPtr->children[0];
                newRoot.oid.pool_uuid_lo = poolUUID;

                pmemobj_free(&persistentRoot.oid);
                persistentRoot = newRoot;

                TOID(PMEMRoot) pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                D_RW(pmemRoot)->persistentRoot = persistentRoot;
                pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));
            }
        }
        return ErrorCode::Success;
    }

    ErrorCode handleUnderflowPersistent(TOID(PersistentNode) parent, TOID(PersistentNode) node) {
        auto* parentPtr = D_RW(parent);
        if (!parentPtr) return ErrorCode::Error;
    
        int index = -1;
        for (int i = 0; i <= parentPtr->keyCount; ++i) {
            if (parentPtr->children[i] == node.oid.off) {
                index = i;
                break;
            }
        }
    
        if (index == -1) return ErrorCode::Error;
    
        TOID(PersistentNode) left, right;
        if (index > 0) {
            left.oid.off = parentPtr->children[index - 1];
            left.oid.pool_uuid_lo = poolUUID;
        }
        if (index + 1 <= parentPtr->keyCount) {
            right.oid.off = parentPtr->children[index + 1];
            right.oid.pool_uuid_lo = poolUUID;
        }
    
        auto* nodePtr = D_RW(node);
        auto* leftPtr = TOID_IS_NULL(left) ? nullptr : D_RW(left);
        auto* rightPtr = TOID_IS_NULL(right) ? nullptr : D_RW(right);
    
        // Try borrow from left
        if (leftPtr && leftPtr->keyCount > m_nDegree / 2) {
            int last = leftPtr->keyCount - 1;
            for (int j = nodePtr->keyCount; j > 0; --j) {
                nodePtr->keys[j] = nodePtr->keys[j - 1];
                nodePtr->children[j] = nodePtr->children[j - 1];
            }
            nodePtr->keys[0] = leftPtr->keys[last];
            nodePtr->children[0] = leftPtr->children[last];
            nodePtr->keyCount++;
    
            leftPtr->keyCount--;
            parentPtr->keys[index - 1] = nodePtr->keys[0];
    
            pmemobj_persist(pmemPoolHandle, nodePtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, leftPtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
            return ErrorCode::Success;
        }
    
        // Try borrow from right
        if (rightPtr && rightPtr->keyCount > m_nDegree / 2) {
            nodePtr->keys[nodePtr->keyCount] = rightPtr->keys[0];
            nodePtr->children[nodePtr->keyCount] = rightPtr->children[0];
            nodePtr->keyCount++;
    
            for (int j = 0; j < rightPtr->keyCount - 1; ++j) {
                rightPtr->keys[j] = rightPtr->keys[j + 1];
                rightPtr->children[j] = rightPtr->children[j + 1];
            }
            rightPtr->keyCount--;
    
            parentPtr->keys[index] = rightPtr->keys[0];
    
            pmemobj_persist(pmemPoolHandle, nodePtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, rightPtr, sizeof(PersistentNode));
            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
            return ErrorCode::Success;
        }
    
        // Merge fallback
        if (!TOID_IS_NULL(left) && index - 1 >= 0) {
            mergeNodes(parent, index - 1);
        } else if (!TOID_IS_NULL(right) && index + 1 <= parentPtr->keyCount) {
            mergeNodes(parent, index);
        } else {
            std::cerr << "[UNDERFLOW] No valid sibling to merge with!\n";
        }

        // After merge, check if root needs to shrink
        if (parent.oid.off == persistentRoot.oid.off) {
            auto* rootPtr = D_RW(parent);
            if (rootPtr->keyCount == 0) {
                persistentRoot.oid.off = rootPtr->children[0];
                pmemobj_persist(pmemPoolHandle, &persistentRoot, sizeof(TOID(PersistentNode)));
    
                TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                D_RW(root)->persistentRoot = persistentRoot;
                pmemobj_persist(pmemPoolHandle, D_RW(root), sizeof(PMEMRoot));
    
                pmemobj_free(&parent.oid);
            }
        }
        return ErrorCode::Success;
    }
    
    
    void mergeNodes(TOID(PersistentNode) parent, int index) {
        auto* parentPtr = D_RW(parent);
    
        if (index < 0 || index + 1 > parentPtr->keyCount) return;
    
        TOID(PersistentNode) left, right;
        left.oid.off = parentPtr->children[index];
        right.oid.off = parentPtr->children[index + 1];
        left.oid.pool_uuid_lo = poolUUID;
        right.oid.pool_uuid_lo = poolUUID;
    
        auto* leftPtr = D_RW(left);
        auto* rightPtr = D_RW(right);
        if (!leftPtr || !rightPtr) return;
    
        // Promote middle key for internal nodes
        if (!leftPtr->isLeaf) {
            leftPtr->keys[leftPtr->keyCount] = parentPtr->keys[index];
            leftPtr->keyCount++;
        }
    
        // Copy keys and children from right ->left
        for (int i = 0; i < rightPtr->keyCount; ++i) {
            leftPtr->keys[leftPtr->keyCount] = rightPtr->keys[i];
            leftPtr->children[leftPtr->keyCount] = rightPtr->children[i];
            leftPtr->keyCount++;
        }
    
        if (!leftPtr->isLeaf) {
            leftPtr->children[leftPtr->keyCount] = rightPtr->children[rightPtr->keyCount];
        }
    
        // Shift parent keys and children
        for (int i = index; i < parentPtr->keyCount - 1; ++i) {
            parentPtr->keys[i] = parentPtr->keys[i + 1];
            parentPtr->children[i + 1] = parentPtr->children[i + 2];
        }
        parentPtr->keyCount--;
    
        // Move buffered keys from right ->left
        uint64_t left_id = getNodeIDFromPersistentNode(left);
        uint64_t right_id = getNodeIDFromPersistentNode(right);
        auto keys = getBufferedKeysForNode(right_id);
        for (auto& k : keys) {
            moveBufferedKeyBetweenNodes(right_id, left_id, k);
            insertToMessageToNodeMap(k, left_id);
        }

        int oldKeyCnt = parentPtr->keyCount; 
        // Free right child
        pmemobj_free(&right.oid);
        parentPtr->children[oldKeyCnt + 1] = 0;
        pmemobj_persist(pmemPoolHandle, leftPtr, sizeof(PersistentNode));
        pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
    
        // Shrink root if needed
        if (parent.oid.off == persistentRoot.oid.off && parentPtr->keyCount == 0) {
            TOID(PersistentNode) newRoot;
            newRoot.oid.off = parentPtr->children[0];
            newRoot.oid.pool_uuid_lo = poolUUID;
    
            pmemobj_free(&persistentRoot.oid);
            persistentRoot = newRoot;
    
            TOID(PMEMRoot) pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(pmemRoot)->persistentRoot = persistentRoot;
            pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));
        }
    
        // Recurse upward if parent underflows and is not root
        else if (parent.oid.off != persistentRoot.oid.off &&
                 parentPtr->keyCount < (m_nDegree / 2)) {
            TOID(PersistentNode) grandparent = findPersistentParent(persistentRoot, TOID_NULL(PersistentNode), parent);
            if (!TOID_IS_NULL(grandparent)) {
                handleUnderflowPersistent(grandparent, parent);
            }
        }
    }

    // UPDATE OP
    ErrorCode update(KeyType key, ValueType newValue)
    {
        if (isReplaying)
            return ErrorCode::Success;

        logOperationBinary(Operations::Update, key, newValue);

        if (TOID_IS_NULL(persistentRoot))
        {
            std::cerr << "[UPDATE] No persistent root exists.\n";
            return ErrorCode::KeyDoesNotExist;
        }

        TOID(PersistentNode)
        target = findTargetNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(target))
        {
            std::cerr << "[UPDATE] ERROR: target node is null.\n";
            return ErrorCode::Error;
        }

        auto *targetPtr = D_RW(target);

        // Tree is only a single leaf node -> update directly
        if (targetPtr->isLeaf && target.oid.off == persistentRoot.oid.off)
        {
            int pos = 0;
            while (pos < targetPtr->keyCount && targetPtr->keys[pos] < key)
                ++pos;

            if (pos < targetPtr->keyCount && targetPtr->keys[pos] == key)
            {
                targetPtr->children[pos] = newValue;
                pmemobj_persist(pmemPoolHandle, targetPtr, sizeof(PersistentNode));
                return ErrorCode::Success;
            }
            else
            {
                std::cerr << "[UPDATE] Key not found in leaf.\n";
                return ErrorCode::KeyDoesNotExist;
            }
        }

        // Internal node path -> buffer the update
        TOID(PersistentNode)
        bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget))
        {
            std::cerr << "[UPDATE] ERROR: Could not find internal node to buffer into.\n";
            return ErrorCode::Error;
        }
        ErrorCode res = insertToNVMSharedBuffer(Operations::Update, key, newValue);
        if (res != ErrorCode::Success)
            return res;

        uint64_t node_id = getNodeIDFromPersistentNode(bufferTarget);
        addKeyToNodeMessageMap(node_id, key);
        insertToMessageToNodeMap(static_cast<uint64_t>(key), node_id);
        incrementNodeFrequency(node_id);

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
            for (int i = 0; i < ptr->keyCount; ++i)
            {
                if (ptr->keys[i] == key)
                {
                    value = static_cast<ValueType>(ptr->children[i]); // values stored in children[]
                    return ErrorCode::Success;
                }
            }
            return ErrorCode::KeyDoesNotExist;
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

        TOID(PersistentNode)
        child;
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
            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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

    TOID(PersistentNode)
    findTargetNodeForKey(TOID(PersistentNode) node, const KeyType &key)
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

        TOID(PersistentNode)
        child;
        child.oid.off = ptr->children[i];
        child.oid.pool_uuid_lo = poolUUID;

        return findTargetNodeForKey(child, key); // recurse
    }

    ErrorCode insert(KeyType key, ValueType value)
    {
        if (isReplaying)
            return ErrorCode::Success; // skip mutation during replay

        logOperationBinary(Operations::Insert, key, value);

        if (TOID_IS_NULL(persistentRoot))
        {
            persistentRoot = allocatePersistentNode(true);
        }

        // std::cout << "[DEBUG] Calling findTargetNodeForKey() on TOID with offset: "
        //           << persistentRoot.oid.off << "\n";

        // 1. Find the deepest node where the key should go
        TOID(PersistentNode)
        target = findTargetNodeForKey(persistentRoot, key);

        if (TOID_IS_NULL(target))
        {
            std::cerr << "[INSERT] ERROR: targetNode is null.\n";
            return ErrorCode::Error;
        }

        auto *targetPtr = D_RW(target);
        // std::cout << "[TRACE] Entering node (isLeaf=" << (int)targetPtr->isLeaf
        //           << ", keyCount=" << targetPtr->keyCount << ")\n";

        if (targetPtr->isLeaf && target.oid.off == persistentRoot.oid.off)
        {
            // std::cout << "[DEBUG] Root is a leaf — inserting directly.\n";

            // Insert directly into leaf
            int pos = 0;
            while (pos < targetPtr->keyCount && targetPtr->keys[pos] < key)
                ++pos;

            // Shift keys to insert
            for (int i = targetPtr->keyCount; i > pos; --i)
            {
                targetPtr->keys[i] = targetPtr->keys[i - 1];
                targetPtr->children[i] = targetPtr->children[i - 1];
            }

            targetPtr->keys[pos] = key;
            targetPtr->children[pos] = value;
            targetPtr->keyCount++;

            pmemobj_persist(pmemPoolHandle, targetPtr, sizeof(PersistentNode));
            // std::cout << "[DEBUG] Inserted into leaf at position " << pos
            //           << ", new keyCount = " << targetPtr->keyCount << "\n";

            // Check for overflow
            if (targetPtr->keyCount >= MAX_KEYS_PER_NODE)
            {
                // std::cout << "[SPLIT] Root leaf full — splitting.\n";
                ErrorCode res = splitPersistentLeaf(TOID_NULL(PersistentNode), target);
                if (res != ErrorCode::Success)
                    return res;
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        // 2. Internal node case — buffer insert
        // std::cout << "[DEBUG] Routing insert to parent internal node for buffering.\n";

        // Find the correct internal node to buffer into
        TOID(PersistentNode)
        bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget))
        {
            std::cerr << "[INSERT] ERROR: Could not find internal node to buffer into.\n";
            return ErrorCode::Error;
        }

        ErrorCode res = insertToNVMSharedBuffer(Operations::Insert, key, value);
        if (res != ErrorCode::Success)
            return res;

        bufferTarget = findTargetInternalNodeForKey(persistentRoot, key);
        if (TOID_IS_NULL(bufferTarget))
        {
            std::cerr << "[INSERT] ERROR: Could not re-find internal node after split.\n";
            return ErrorCode::Error;
        }

        uint64_t node_id = getNodeIDFromPersistentNode(bufferTarget);
        addKeyToNodeMessageMap(node_id, key);
        insertToMessageToNodeMap(static_cast<uint64_t>(key), node_id);
        incrementNodeFrequency(node_id);

        maybeCheckpoint();
        return ErrorCode::Success;
    }

    TOID(PersistentNode)
    findTargetInternalNodeForKey(TOID(PersistentNode) node, KeyType key)
    {
        if (TOID_IS_NULL(node))
            return TOID_NULL(PersistentNode);
        auto *ptr = D_RO(node);

        if (!ptr)
        {
            std::cerr << "[ERROR] findTargetInternalNodeForKey(): D_RO returned nullptr.\n";
            return TOID_NULL(PersistentNode);
        }

        if (ptr->isLeaf)
            return TOID_NULL(PersistentNode); // Avoid returning leaf

        int i = 0;
        while (i < ptr->keyCount && key >= ptr->keys[i])
            ++i;
        if (i > ptr->keyCount || i >= MAX_KEYS_PER_NODE || ptr->children[i] == 0)
        {
            std::cerr << "[ERROR] Invalid child index or null child pointer at index " << i << "\n";
            return TOID_NULL(PersistentNode);
        }

        TOID(PersistentNode)
        child;
        child.oid.off = ptr->children[i];
        child.oid.pool_uuid_lo = poolUUID;

        auto *childPtr = D_RO(child);
        if (!childPtr)
        {
            std::cerr << "[ERROR] D_RO(child) returned null for offset " << ptr->children[i] << "\n";
            return TOID_NULL(PersistentNode);
        }

        if (childPtr->isLeaf)
            return node;
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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

        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);

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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto *auxPtr = D_RW(auxRoot);

        if (auxPtr->nodeMessageMapCount == 0)
            return ErrorCode::Success;

        // 1. Find node with most buffered keys
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

        TOID(PersistentNode)
        originalTarget = findNodeById(persistentRoot, node_id);
        if (TOID_IS_NULL(originalTarget))
        {
            std::cerr << "[FLUSH] Failed to locate PersistentNode with ID " << node_id << "\n";
            return ErrorCode::Error;
        }

        // std::cout << "[FLUSH] Flushing persistent node ID " << node_id << "\n";

        if (D_RO(originalTarget)->isLeaf)
        {
            return ErrorCode::Success;
        }

        // Re-fetch key list since original pointer is const
        auto bufferedKeys = getBufferedKeysForNode(node_id);
        for (const auto &key : bufferedKeys)
        {
            auto messageOpt = lookupInNVMBuffer(key);
            if (!messageOpt.has_value())
                continue;

            auto [op, val] = messageOpt.value();

            // Recompute parent after every key, to account for splits
            TOID(PersistentNode)
            flushParent = findTargetInternalNodeForKey(persistentRoot, key);
            if (TOID_IS_NULL(flushParent))
            {
                std::cerr << "[FLUSH] Could not find parent for key " << key << "\n";
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

            TOID(PersistentNode) child;
            child.oid.off = parentPtr->children[i];
            child.oid.pool_uuid_lo = poolUUID;
            auto *childPtr = D_RW(child);

            if (childPtr->isLeaf)
            {
                int pos = 0;
                while (pos < childPtr->keyCount && childPtr->keys[pos] < key)
                    ++pos;

                if (op == Operations::Insert || op == Operations::Update)
                {
                    if (pos < childPtr->keyCount && childPtr->keys[pos] == key)
                {
                    childPtr->children[pos] = val;
                    pmemobj_persist(pmemPoolHandle, childPtr, sizeof(PersistentNode));

                    // Check for leaf overflow even after update
                    if (childPtr->keyCount >= MAX_KEYS_PER_NODE)
                    {
                        ErrorCode res = splitPersistentLeaf(flushParent, child);
                        if (res != ErrorCode::Success)
                        {
                            std::cerr << "[ERROR] Failed to split leaf after update.\n";
                            return res;
                        }
                    }

                    // Also check if root internal node is full
                    if (flushParent.oid.off == persistentRoot.oid.off)
                    {
                        auto *rootPtr = D_RW(persistentRoot);
                        if (rootPtr->keyCount >= MAX_KEYS_PER_NODE)
                        {
                            splitPersistentNode(TOID_NULL(PersistentNode), persistentRoot);
                        }
                    }

                    // Now cleanup metadata and buffer
                    removeKeyFromAllNodeMessageMaps(key);
                    removeKeyFromMessageToNodeMap(key);
                    TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                    auto *rootPtr = D_RW(root);
                    for (int i = 0; i < rootPtr->messageCount; ++i)
                    {
                        auto *msg = D_RW(rootPtr->messages[i]);
                        if (msg->key_size == sizeof(KeyType) &&
                            std::memcmp(msg->key_data, &key, sizeof(KeyType)) == 0)
                        {
                            pmemobj_free(&rootPtr->messages[i].oid);
                            for (int j = i + 1; j < rootPtr->messageCount; ++j)
                                rootPtr->messages[j - 1] = rootPtr->messages[j];
                            rootPtr->messageCount--;
                            pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));
                            break;
                        }
                    }

                    continue;
                }

                    for (int j = childPtr->keyCount; j > pos; --j)
                    {
                        childPtr->keys[j] = childPtr->keys[j - 1];
                        childPtr->children[j] = childPtr->children[j - 1];
                    }
                    childPtr->keys[pos] = key;
                    childPtr->children[pos] = val;
                    childPtr->keyCount++;
                    pmemobj_persist(pmemPoolHandle, childPtr, sizeof(PersistentNode));

                    if (childPtr->keyCount >= MAX_KEYS_PER_NODE)
                    {
                        // std::cout << "[SPLIT] Leaf node full after flush — splitting.\n";
                        ErrorCode res = splitPersistentLeaf(flushParent, child);
                        if (res != ErrorCode::Success)
                        {
                            std::cerr << "[ERROR] Failed to split leaf after flush.\n";
                            return res;
                        }
                    }
                    if (flushParent.oid.off == persistentRoot.oid.off)
                    {
                        // std::cout << "[DEBUG] Root might be overfull — checking split condition...\n";
                        auto *rootPtr = D_RW(persistentRoot);
                        if (rootPtr->keyCount >= MAX_KEYS_PER_NODE)
                        {
                            // std::cout << "[SPLIT] Root internal node full — splitting.\n";
                            splitPersistentNode(TOID_NULL(PersistentNode), persistentRoot);
                        }
                    }
                    // Check if internal node (flushParent) needs splitting before inserting
                    if (parentPtr->keyCount >= MAX_KEYS_PER_NODE)
                    {
                        // std::cout << "[SPLIT] Parent internal node full — splitting before flush insert.\n";
                        TOID(PersistentNode)
                        grandparent = findPersistentParent(persistentRoot, flushParent, originalTarget);
                        ErrorCode res = splitPersistentNode(grandparent, flushParent);
                        if (res != ErrorCode::Success)
                        {
                            std::cerr << "[ERROR] Failed to split internal parent during flush.\n";
                            return res;
                        }

                        // Re-locate parent & child after structural change
                        flushParent = findTargetInternalNodeForKey(persistentRoot, key);
                        if (TOID_IS_NULL(flushParent))
                        {
                            std::cerr << "[ERROR] Could not re-find internal node after split.\n";
                            return ErrorCode::Error;
                        }
                        parentPtr = D_RW(flushParent);

                        i = 0;
                        while (i < parentPtr->keyCount && key >= parentPtr->keys[i])
                            ++i;
                        if (i > MAX_KEYS_PER_NODE || parentPtr->children[i] == 0)
                        {
                            std::cerr << "[ERROR] Invalid child index or null child pointer at index " << i << "\n";
                            return ErrorCode::Error;
                        }
                    }
                }
                else if (op == Operations::Delete)
                {
                    for (int j = pos; j < childPtr->keyCount - 1; ++j)
                    {
                        childPtr->keys[j] = childPtr->keys[j + 1];
                        childPtr->children[j] = childPtr->children[j + 1];
                    }
                    childPtr->keyCount--;
                    pmemobj_persist(pmemPoolHandle, childPtr, sizeof(PersistentNode));

                    if (childPtr->keyCount < (m_nDegree / 2)) {
                        std::cout << "[UNDERFLOW DETECTED] child node " << child.oid.off
                        << " has keyCount = " << childPtr->keyCount << "\n";

                        ErrorCode result = handleUnderflowPersistent(flushParent, child);
                        if (result != ErrorCode::Success) {
                            std::cerr << "[UNDERFLOW] Failed to handle underflow after delete.\n";
                            return result;
                        }
                    }
                }
            }
            else
            {
                // Skip rebuffering because operation is already applied during flush
                // std::cout << "[DEBUG] Skipping rebuffer for key " << key << " after flush\n";
                continue;
            }

            removeKeyFromAllNodeMessageMaps(key);
            removeKeyFromMessageToNodeMap(key);

            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            auto *rootPtr = D_RW(root);
            for (int i = 0; i < rootPtr->messageCount; ++i)
            {
                auto *msg = D_RW(rootPtr->messages[i]);
                if (msg->key_size == sizeof(KeyType) &&
                    std::memcmp(msg->key_data, &key, sizeof(KeyType)) == 0)
                {
                    pmemobj_free(&rootPtr->messages[i].oid);
                    for (int j = i + 1; j < rootPtr->messageCount; ++j)
                        rootPtr->messages[j - 1] = rootPtr->messages[j];
                    rootPtr->messageCount--;
                    pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));

                    break;
                }
            }
        }
        if (!TOID_IS_NULL(persistentRoot)) {
            auto* rootPtr = D_RW(persistentRoot);
            if (!rootPtr->isLeaf && rootPtr->keyCount == 0) {
                TOID(PersistentNode) newRoot;
                newRoot.oid.off = rootPtr->children[0];
                newRoot.oid.pool_uuid_lo = poolUUID;

                pmemobj_free(&persistentRoot.oid);
                persistentRoot = newRoot;

                TOID(PMEMRoot) pmemRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
                D_RW(pmemRoot)->persistentRoot = persistentRoot;
                pmemobj_persist(pmemPoolHandle, D_RW(pmemRoot), sizeof(PMEMRoot));

                std::cout << "[SHRINK] Root shrunk to child node with offset " << newRoot.oid.off << "\n";
            }
        }


        return ErrorCode::Success;
    }

    TOID(PersistentNode)
    findNodeById(TOID(PersistentNode) node, uint64_t node_id) const
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
            TOID(PersistentNode)
            child;
            child.oid.off = ptr->children[i];
            child.oid.pool_uuid_lo = poolUUID;

            TOID(PersistentNode)
            found = findNodeById(child, node_id);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        addKeyToNodeMessageMap(to_id, key);
    }

    void insertToMessageToNodeMap(uint64_t key, uint64_t node_id)
    {
        removeKeyFromMessageToNodeMap(key);
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
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
        node->children[index] = child.oid.off;
        pmemobj_persist(pmemPoolHandle, &node->children[index], sizeof(uint64_t));
    }

    ErrorCode splitPersistentLeaf(TOID(PersistentNode) parent, TOID(PersistentNode) leaf)
    {
        if (TOID_IS_NULL(leaf))
        {
            std::cerr << "[SPLIT] ERROR: Leaf is null!\n";
            return ErrorCode::Error;
        }

        auto *leafPtr = D_RW(leaf);
        if (!leafPtr->isLeaf)
        {
            std::cerr << "[SPLIT] ERROR: Not a leaf node.\n";
            return ErrorCode::Error;
        }

        int mid = leafPtr->keyCount / 2;

        // Allocate sibling
        TOID(PersistentNode)
        sibling = allocatePersistentNode(true);
        if (TOID_IS_NULL(sibling))
        {
            std::cerr << "[SPLIT] ERROR: Failed to allocate sibling leaf.\n";
            return ErrorCode::Error;
        }

        auto *siblingPtr = D_RW(sibling);
        siblingPtr->keyCount = leafPtr->keyCount - mid;

        // Copy upper half to sibling
        for (int i = 0; i < siblingPtr->keyCount; ++i)
        {
            siblingPtr->keys[i] = leafPtr->keys[mid + i];
            siblingPtr->children[i] = leafPtr->children[mid + i];
        }

        // Shrink original leaf
        leafPtr->keyCount = mid;

        pmemobj_persist(pmemPoolHandle, leafPtr, sizeof(PersistentNode));
        pmemobj_persist(pmemPoolHandle, siblingPtr, sizeof(PersistentNode));

        uint64_t pivot = siblingPtr->keys[0];

        // Fix message mappings for keys ≥ pivot
        uint64_t from_id = getNodeIDFromPersistentNode(leaf);
        uint64_t to_id = getNodeIDFromPersistentNode(sibling);

        auto movedKeys = getBufferedKeysForNode(from_id);
        for (const auto &k : movedKeys)
        {
            if (k >= pivot)
            {
                moveBufferedKeyBetweenNodes(from_id, to_id, k);
                insertToMessageToNodeMap(static_cast<uint64_t>(k), to_id);
                // std::cout << "[DEBUG] Moved buffered key " << k << " -> new sibling leaf\n";
            }
        }

        // Promote pivot to parent
        if (TOID_IS_NULL(parent))
        {
            TOID(PersistentNode)
            newRoot = allocatePersistentNode(false);
            if (TOID_IS_NULL(newRoot))
                return ErrorCode::Error;

            auto *rootPtr = D_RW(newRoot);
            rootPtr->keys[0] = pivot;
            rootPtr->keyCount = 1;
            setChildPointer(rootPtr, 0, leaf);
            setChildPointer(rootPtr, 1, sibling);

            persistentRoot = newRoot;
            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(root)->persistentRoot = persistentRoot;
            pmemobj_persist(pmemPoolHandle, D_RW(root), sizeof(PMEMRoot));
        }
        else
        {
            auto *parentPtr = D_RW(parent);

            // Shift keys and children to make space
            int i = parentPtr->keyCount;
            while (i > 0 && parentPtr->keys[i - 1] > pivot)
            {
                parentPtr->keys[i] = parentPtr->keys[i - 1];
                parentPtr->children[i + 1] = parentPtr->children[i];
                i--;
            }

            parentPtr->keys[i] = pivot;
            setChildPointer(parentPtr, i + 1, sibling);
            parentPtr->keyCount++;

            pmemobj_persist(pmemPoolHandle, parentPtr, sizeof(PersistentNode));
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

        TOID(PersistentNode)
        sibling = allocatePersistentNode(false);
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
                // std::cout << "[DEBUG] Moved buffered key " << k << " -> sibling during internal split\n";
            }
        }

        // Promote to root if no parent
        if (TOID_IS_NULL(parent))
        {
            TOID(PersistentNode)
            newRoot = allocatePersistentNode(false);
            if (TOID_IS_NULL(newRoot))
                return ErrorCode::Error;
            auto *rootPtr = D_RW(newRoot);

            rootPtr->keys[0] = pivotKey;
            rootPtr->keyCount = 1;
            setChildPointer(rootPtr, 0, node);
            setChildPointer(rootPtr, 1, sibling);

            persistentRoot = newRoot;
            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            D_RW(root)->persistentRoot = persistentRoot;
            pmemobj_persist(pmemPoolHandle, D_RW(root), sizeof(PMEMRoot));
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
                TOID(PersistentNode)
                grandparent = findPersistentParent(persistentRoot, parent, node);
                return splitPersistentNode(grandparent, parent);
            }
        }

        return ErrorCode::Success;
    }

    TOID(PersistentNode)findPersistentParent(TOID(PersistentNode) current, TOID(PersistentNode) target, TOID(PersistentNode) childToFind)
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

            TOID(PersistentNode)
            child;
            child.oid.off = curPtr->children[i];
            child.oid.pool_uuid_lo = poolUUID;

            TOID(PersistentNode)
            result = findPersistentParent(child, target, childToFind);
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

                TOID(PersistentNode)
                child;
                child.oid.off = ptr->children[i];
                child.oid.pool_uuid_lo = poolUUID;

                rangeQueryRecursive(child, low, high, result);
            }
        }
    }

    void displayPersistentTree(TOID(PersistentNode) node, int level = 0)
    {
        if (TOID_IS_NULL(node))
            return;

        auto *ptr = D_RO(node);
        if (!ptr)
            return;

        std::cout << std::string(level * 2, ' ') << "[";
        for (int i = 0; i < ptr->keyCount; ++i)
        {
            if (ptr->isLeaf)
            {
                std::cout << ptr->keys[i] << ":" << ptr->children[i] << " ";
            }
            else
            {
                std::cout << ptr->keys[i] << " ";
            }
        }
        std::cout << "]\n";

        if (!ptr->isLeaf)
        {
            for (int i = 0; i <= ptr->keyCount; ++i)
            {
                if (ptr->children[i] == 0)
                    continue; // Skip nulls

                TOID(PersistentNode)
                child;
                child.oid.off = ptr->children[i];
                child.oid.pool_uuid_lo = poolUUID;

                displayPersistentTree(child, level + 1);
            }
        }
    }

    std::string nodeKeySummary(uint64_t node_id) const
    {
        TOID(PersistentNode)
        node = findNodeById(persistentRoot, node_id);
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

};