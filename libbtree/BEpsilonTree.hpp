#pragma once
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
    std::shared_ptr<Node<KeyType, ValueType>> root;
    PMEMobjpool* pmemPoolHandle = nullptr;  // NVM pool

    uint32_t m_nDegree;
    std::string logFilename = getPlatformPath("nvm_tree_test1.log");
    int opCounter = 0;
    int checkpointFrequency = 5;
    bool isReplaying = false;

    // DRAM read buffer
    std::unordered_map<KeyType, ValueType> readCache;
    std::list<KeyType> lruList;
    size_t maxReadCacheSize = 100;

    BEpsilonTree(int degree, const std::string& filename, int checkpointFreq = -1)
    {
        this->m_nDegree = degree;
        if (checkpointFreq > 0)
            this->checkpointFrequency = checkpointFreq;

        // Try loading tree from file
        std::ifstream inFile(filename, std::ios::binary);
        if (inFile)
        {
            root = loadTreeFromFile(filename);
        }
        else
        {
            root = std::make_shared<Node<KeyType, ValueType>>(true); // New tree
        }

        // === Initialize NVM Pool for shared buffer ===
        const std::string pmemPath = getPlatformPath("shared_buffer_pool.pmem");

        std::cout << "[DEBUG] Trying to open PMEM pool at: " << pmemPath << "\n";
        initializePMEMPool(pmemPath);
        replayBinaryWAL();
    }

    ~BEpsilonTree()
    {
        // Save final tree
        saveTreeToFile(root, getPlatformPath("tree_data.bin"));

        // Checkpoint if we have outstanding ops
        if (opCounter > 0) {
            checkpoint();
        }

        // Backup and clear binary WAL
        const std::string binWal = getPlatformPath("nvm_tree_bin.wal");
        const std::string bakName = getPlatformPath("wal_bin_backup") + timestampename() + ".bak";

        std::ifstream src(binWal, std::ios::binary);
        std::ofstream dst(bakName, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();  // Backup WAL
        }
        src.close();
        dst.close();

        // Truncate original WAL
        std::ofstream clear(binWal, std::ios::trunc | std::ios::binary);
        clear.close();

        if (pmemPoolHandle) {
            pmemobj_close(pmemPoolHandle);
            pmemPoolHandle = nullptr;
        }
        
    }


private:
    void replayBinaryWAL() {
        const std::string walPath = getPlatformPath("nvm_tree_bin.wal");
        std::ifstream walBin(walPath, std::ios::binary);
        if (!walBin.is_open()) {
            std::cout << "[WAL] No WAL file to replay.\n";
            return;
        }

        std::cout << "[WAL] Replaying binary WAL from: " << walPath << "\n";
        isReplaying = true;

        while (!walBin.eof()) {
            uint8_t opCode;
            KeyType key;
            ValueType value;

            walBin.read(reinterpret_cast<char*>(&opCode), sizeof(opCode));
            if (walBin.eof()) break;

            walBin.read(reinterpret_cast<char*>(&key), sizeof(KeyType));
            Operations op = static_cast<Operations>(opCode);

            if (op == Operations::Insert || op == Operations::Update) {
                walBin.read(reinterpret_cast<char*>(&value), sizeof(ValueType));
            }

            switch (op) {
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

    void initializePMEMPool(const std::string& pmemPath) {
        #ifdef _WIN32
        std::wstring pmemPathW(pmemPath.begin(), pmemPath.end());
        pmemPoolHandle = pmemobj_openW(pmemPathW.c_str(), LAYOUT_NAME);
        #else
        pmemPoolHandle = pmemobj_open(pmemPath.c_str(), LAYOUT_NAME);
        #endif
        
        if (!pmemPoolHandle) {
            std::cout << "[DEBUG] Pool not found. Creating new PMEM pool...\n";
            #ifdef _WIN32
            pmemPoolHandle = pmemobj_createW(pmemPathW.c_str(), LAYOUT_NAME, 64 * 1024 * 1024, 0666);
            #else
            pmemPoolHandle = pmemobj_create(pmemPath.c_str(), LAYOUT_NAME, 64 * 1024 * 1024, 0666);
            #endif
            if (!pmemPoolHandle) {
                std::cerr << "[ERROR] Failed to create PMEM pool! Exiting.\n";
                perror("pmemobj_create");
                exit(1);
            }

            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            auto* ptr = D_RW(root);

            // Initialize buffer + maps
            ptr->messageCount = 0;
            for (int i = 0; i < MAX_NVM_MESSAGES; ++i) {
                ptr->messages[i] = TOID_NULL(message);
            }
            ptr->nodeFrequencyCount = 0;
            ptr->nodeMessageMapCount = 0;
            ptr->messageToNodeMapCount = 0;

            pmemobj_persist(pmemPoolHandle, ptr, sizeof(PMEMRoot));
            std::cout << "[NVM] New PMEM pool created successfully.\n";
        } else {
            std::cout << "[NVM] Existing PMEM pool opened successfully.\n";
        }
    }
    
    void incrementNodeFrequency(uint64_t node_id) {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->nodeFrequencyCount; ++i) {
            auto* entry = D_RW(auxPtr->nodeFrequencyMap[i]);
            if (entry->node_id == node_id) {
                entry->frequency += 1;
                pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeFrequencyEntry));
                return;
            }
        }

        if (auxPtr->nodeFrequencyCount < 128) {
            TOID(NodeFrequencyEntry) newEntry;
            if (pmemobj_alloc(pmemPoolHandle, &newEntry.oid, sizeof(NodeFrequencyEntry), 0, nullptr, nullptr) == 0) {
                D_RW(newEntry)->node_id = node_id;
                D_RW(newEntry)->frequency = 1;
                pmemobj_persist(pmemPoolHandle, D_RW(newEntry), sizeof(NodeFrequencyEntry));
                auxPtr->nodeFrequencyMap[auxPtr->nodeFrequencyCount++] = newEntry;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot)); 
            }
        }
    }

    ErrorCode insertToNVMSharedBuffer(Operations op, const KeyType& key, const ValueType& value = ValueType{}) 
{
    if (!pmemPoolHandle) return ErrorCode::Error;

    TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
    auto* rootPtr = D_RW(root);

    // Search for existing message on the same key
    int existingIndex = -1;
    for (int i = 0; i < rootPtr->messageCount; ++i) {
        message* msgPtr = D_RW(rootPtr->messages[i]);
        if (msgPtr->key_size == sizeof(KeyType) &&
            std::memcmp(msgPtr->key_data, &key, sizeof(KeyType)) == 0) {
            existingIndex = i;
            break;
        }
    }

    // === COALESCING LOGIC ===
    if (existingIndex != -1) {
        message* msgPtr = D_RW(rootPtr->messages[existingIndex]);
        Operations prevOp = static_cast<Operations>(msgPtr->opCode);

        if (prevOp == Operations::Insert && op == Operations::Delete) {
            // Remove message
            for (int j = existingIndex + 1; j < rootPtr->messageCount; ++j)
                rootPtr->messages[j - 1] = rootPtr->messages[j];
            rootPtr->messageCount--;
            pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));

            // Clean metadata
            auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
            if (nodeIdOpt.has_value()) {
                removeKeyFromNodeMessageMap(nodeIdOpt.value(), key);
            }
            removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));
            return ErrorCode::Success;
        }

        // Regular overwrite (Insert/Update)
        msgPtr->opCode = static_cast<uint8_t>(op);
        msgPtr->key_size = sizeof(KeyType);
        msgPtr->val_size = (op == Operations::Delete) ? 0 : sizeof(ValueType);
        std::memcpy(msgPtr->key_data, &key, sizeof(KeyType));
        if (op != Operations::Delete) {
            std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
        }
        pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
        return ErrorCode::Success;
    }

    // === HANDLE INSERTION OF NEW DELETE ===
    if (op == Operations::Delete) {
        // Message does not exist, but key might still be in maps → clean stale metadata
        auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
        if (nodeIdOpt.has_value()) {
            removeKeyFromNodeMessageMap(nodeIdOpt.value(), key);
        }
        removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));
        return ErrorCode::Success;  // nothing more to store
    }

    // === FLUSH IF FULL ===
    while (rootPtr->messageCount >= MAX_NVM_MESSAGES) {
        auto flushResult = flushMostBufferedNode();
        if (flushResult != ErrorCode::Success) {
            std::cerr << "[ERROR] Failed to flush any node. Shared buffer stuck.\n";
            return ErrorCode::Error;
        }

        root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        rootPtr = D_RW(root);
    }


    auto nodeIdOpt = lookupNodeIdForKey(static_cast<uint64_t>(key));
    if (nodeIdOpt.has_value()) {
        removeKeyFromNodeMessageMap(nodeIdOpt.value(), key);
    }
    removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));

    // === ALLOCATE NEW MESSAGE ===
    TOID(message) msg;
    int alloc_status = pmemobj_alloc(pmemPoolHandle, &msg.oid, sizeof(message), 0, nullptr, nullptr);
    if (alloc_status != 0 || TOID_IS_NULL(msg)) {
        std::cerr << "[NVM] Failed to allocate message in PMEM (status = " << alloc_status << ").\n";
        return ErrorCode::Error;
    }

    auto* msgPtr = D_RW(msg);
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
    void updateReadCache(const KeyType& key, const ValueType& value) {
        // If already in cache -> move to front (MRU)
        if (readCache.find(key) != readCache.end()) {
            lruList.remove(key);
        }
        else {
            // Not in cache -> check capacity
            if (readCache.size() >= maxReadCacheSize) {
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
    std::string timestampename() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utcTime;
#ifdef _WIN32
        gmtime_s(&utcTime, &time);  // Windows
#else
        gmtime_r(&time, &utcTime);  // Linux/Unix
#endif
        std::stringstream timestamp;
        timestamp << std::put_time(&utcTime, "%Y-%m-%d_%H-%M-%S");
        return timestamp.str();
    }

    void checkpoint() {
        // Save tree to disk
        saveTreeToFile(root, getPlatformPath("tree_data.bin"));

        std::string backupFilename = getPlatformPath("wal_backup") + timestampename() + ".bak";

        // Copy textual WAL to a backup
        std::ifstream src(logFilename, std::ios::binary);
        std::ofstream dst(backupFilename, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();
        }
        src.close();
        dst.close();

        // Clear the textual WAL
        std::ofstream clearLog(logFilename, std::ios::trunc);
        clearLog.close();

        // todo
        // std::cout << "[CHECKPOINT] Tree saved, WAL cleared, backup created: " << backupFilename << "\n";
    }

    void maybeCheckpoint() {
        if (isReplaying) return; // Avoid checkpointing mid-replay

        if (++opCounter >= checkpointFrequency) {
            checkpoint();
            opCounter = 0;
        }
    }

    void logOperationBinary(Operations op, const KeyType& key, const ValueType& value = ValueType{}) {
        if (isReplaying) return;
        std::ofstream log(getPlatformPath("nvm_tree_bin.wal"),
            std::ios::binary | std::ios::app);
        if (!log.is_open()) {
            std::cerr << "Failed to open binary WAL.\n";
            return;
        }

        uint8_t opCode = static_cast<uint8_t>(op);
        log.write(reinterpret_cast<const char*>(&opCode), sizeof(opCode));
        log.write(reinterpret_cast<const char*>(&key), sizeof(KeyType));

        if (op == Operations::Insert || op == Operations::Update) {
            log.write(reinterpret_cast<const char*>(&value), sizeof(ValueType));
        }
        log.close();
    }

    // Delete entire tree (unused, but included for completeness)
    void deleteTree(std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        if (!node) return;
        for (auto& child : node->children) {
            deleteTree(child);
        }
        node.reset();
    }

    // Save entire tree to a file (level-order)
    void saveTreeToFile(const std::shared_ptr<Node<KeyType, ValueType>>& root,
        const std::string& filename)
    {
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile) {
            std::cerr << "Error: Could not create file '" << filename << "' for writing!\n";
            return;
        }

        std::queue<std::shared_ptr<Node<KeyType, ValueType>>> q;
        q.push(root);

        while (!q.empty())
        {
            auto node = q.front();
            q.pop();

            bool isLeaf = node->isLeaf;
            size_t keyCount = node->keys.size();

            outFile.write(reinterpret_cast<char*>(&isLeaf), sizeof(bool));
            outFile.write(reinterpret_cast<char*>(&keyCount), sizeof(size_t));
            outFile.write(reinterpret_cast<const char*>(node->keys.data()), keyCount * sizeof(KeyType));

            if (node->isLeaf)
            {
                size_t valueCount = node->values.size();
                outFile.write(reinterpret_cast<char*>(&valueCount), sizeof(size_t));
                outFile.write(reinterpret_cast<const char*>(node->values.data()), valueCount * sizeof(ValueType));
            }
            else
            {
                size_t childCount = node->children.size();
                outFile.write(reinterpret_cast<char*>(&childCount), sizeof(size_t));
                for (auto& child : node->children)
                {
                    q.push(child);
                }
            }
        }
        outFile.close();
    }

    // Load entire tree from a file
    std::shared_ptr<Node<KeyType, ValueType>> loadTreeFromFile(const std::string& filename)
    {
        std::ifstream inFile(filename, std::ios::binary);
        if (!inFile) {
            std::cerr << "Error opening file for reading!\n";
            return nullptr;
        }

        auto root = std::make_shared<Node<KeyType, ValueType>>(true);
        std::queue<std::shared_ptr<Node<KeyType, ValueType>>> q;
        q.push(root);

        while (!q.empty())
        {
            auto node = q.front();
            q.pop();

            bool isLeaf;
            size_t keyCount;
            inFile.read(reinterpret_cast<char*>(&isLeaf), sizeof(bool));
            inFile.read(reinterpret_cast<char*>(&keyCount), sizeof(size_t));
            if (inFile.eof() || inFile.fail()) {
                std::cerr << "Error reading node metadata! Possibly corrupt.\n";
                return nullptr;
            }

            node->isLeaf = isLeaf;
            node->keys.resize(keyCount);
            inFile.read(reinterpret_cast<char*>(node->keys.data()), keyCount * sizeof(KeyType));

            if (isLeaf) {
                size_t valueCount;
                inFile.read(reinterpret_cast<char*>(&valueCount), sizeof(size_t));
                if (valueCount != keyCount) {
                    std::cerr << "ERROR: Value count != key count! " << valueCount << " vs. " << keyCount << "\n";
                    return nullptr;
                }
                node->values.resize(valueCount);
                inFile.read(reinterpret_cast<char*>(node->values.data()), valueCount * sizeof(ValueType));
            }
            else {
                size_t childCount;
                inFile.read(reinterpret_cast<char*>(&childCount), sizeof(size_t));
                for (size_t i = 0; i < childCount; ++i) {
                    auto child = std::make_shared<Node<KeyType, ValueType>>(true);
                    node->children.push_back(child);
                    q.push(child);
                }
                node->isLeaf = false;
            }
        }
        inFile.close();
        return root;
    }

    KeyType getNodeKey(const std::shared_ptr<Node<KeyType, ValueType>>& node)
    {
        return (!node->keys.empty()) ? node->keys[0] : KeyType{};
    }

public:
    // === DELETE OP ===

    ErrorCode remove(KeyType key) {
        if (!root)
            return ErrorCode::KeyDoesNotExist; // Empty tree

        if (isReplaying)
            return ErrorCode::Success;  // Prevent mutation during WAL replay

        logOperationBinary(Operations::Delete, key);

        auto current = root;
        if (!current->isLeaf)
        {
            // Insert a "Delete" into the buffer of the internal node
            ErrorCode result = insertBuffered(current, Operations::Delete, key, ValueType{});
            if (result != ErrorCode::Success) return result;

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        // If we are in a leaf node, remove key directly
        auto it = std::find(current->keys.begin(), current->keys.end(), key);
        if (it != current->keys.end())
        {
            size_t index = std::distance(current->keys.begin(), it);
            current->keys.erase(it);
            current->values.erase(current->values.begin() + index);
        }
        else
        {
            return ErrorCode::KeyDoesNotExist; // Key not found
        }

        // Check underflow
        if (current->keys.size() < (m_nDegree / 2))
        {
            auto parent = findParent(root, current);
            ErrorCode res = handleUnderflow(parent, current);
            if (res != ErrorCode::Success) return res;
        }

        maybeCheckpoint();
        return ErrorCode::Success;
    }

    // Handle underflow in a node (classic B-tree merges/borrows)
    ErrorCode handleUnderflow(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        if (!parent) return ErrorCode::Success; // Root underflow is a special case

        // Find index of 'node' in parent's children
        size_t index = std::find(parent->children.begin(), parent->children.end(), node)
            - parent->children.begin();
        std::shared_ptr<Node<KeyType, ValueType>> leftSibling = (index > 0)
            ? parent->children[index - 1] : nullptr;
        std::shared_ptr<Node<KeyType, ValueType>> rightSibling = (index + 1 < parent->children.size())
            ? parent->children[index + 1] : nullptr;

        // Try borrow from left
        if (leftSibling && leftSibling->keys.size() > (m_nDegree / 2))
        {
            // Borrow largest key from left
            KeyType borrowedKey = leftSibling->keys.back();
            ValueType borrowedValue = leftSibling->values.back();

            leftSibling->keys.pop_back();
            leftSibling->values.pop_back();

            node->keys.insert(node->keys.begin(), borrowedKey);
            node->values.insert(node->values.begin(), borrowedValue);

            // Update parent's separator
            parent->keys[index - 1] = borrowedKey;

            return ErrorCode::Success;
        }

        // Try borrow from right
        if (rightSibling && rightSibling->keys.size() > (m_nDegree / 2))
        {
            // Borrow smallest key from right
            KeyType borrowedKey = rightSibling->keys.front();
            ValueType borrowedValue = rightSibling->values.front();

            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->values.erase(rightSibling->values.begin());

            node->keys.push_back(borrowedKey);
            node->values.push_back(borrowedValue);

            // Update parent's separator
            parent->keys[index] = rightSibling->keys.front();

            return ErrorCode::Success;
        }

        // Merge
        if (leftSibling) {
            mergeNodes(parent, leftSibling, node, index - 1);
        }
        else if (rightSibling) {
            mergeNodes(parent, node, rightSibling, index);
        }

        return ErrorCode::Success;
    }

    // Merge two nodes
    void mergeNodes(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> left,
        std::shared_ptr<Node<KeyType, ValueType>> right,
        size_t separatorIndex)
    {
        if (!left || !right || !parent) return;
        if (separatorIndex >= parent->keys.size()) {
            std::cerr << "[ERROR] Invalid separator index in merge.\n";
            return;
        }

        // If internal, push down parent's separator key
        if (!left->isLeaf)
            left->keys.push_back(parent->keys[separatorIndex]);

        // Merge keys/values
        left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
        left->values.insert(left->values.end(), right->values.begin(), right->values.end());

        // Merge children if internal
        if (!left->isLeaf && !right->isLeaf) {
            left->children.insert(left->children.end(), right->children.begin(), right->children.end());
        }

        // Remove key/child from parent
        parent->keys.erase(parent->keys.begin() + separatorIndex);
        parent->children.erase(parent->children.begin() + separatorIndex + 1);

        // Reassign messages
        uint64_t right_id = static_cast<uint64_t>(getNodeKey(right));
        uint64_t left_id = static_cast<uint64_t>(getNodeKey(left));

        auto keys = getBufferedKeysForNode(right_id);
        for (auto& k : keys) {
            moveBufferedKeyBetweenNodes(right_id, left_id, k);
            insertToMessageToNodeMap(static_cast<uint64_t>(k), left_id);

        }
        right.reset();
    }

    // === UPDATE OP ===
    ErrorCode update(KeyType key, ValueType newValue)
    {
        if (isReplaying) return ErrorCode::Success; // skip actual changes during replay

        logOperationBinary(Operations::Update, key, newValue);
        auto current = root;
        if (!current->isLeaf)
        {
            ErrorCode result = insertBuffered(current, Operations::Update, key, newValue);
            if (result != ErrorCode::Success) return result;

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        // Leaf node update
        auto it = std::find(current->keys.begin(), current->keys.end(), key);
        if (it != current->keys.end())
        {
            size_t idx = std::distance(current->keys.begin(), it);
            current->values[idx] = newValue;
        }
        else {
            return ErrorCode::KeyDoesNotExist;
        }

        maybeCheckpoint();
        return ErrorCode::Success;
    }

    // === SEARCH OP ===
    ErrorCode search(KeyType key, ValueType& value)
    {
        // 1. Check DRAM read cache
        auto cacheIt = readCache.find(key);
        if (cacheIt != readCache.end()) {
            lruList.remove(key);
            lruList.push_front(key);
            value = cacheIt->second;
            return ErrorCode::Success;
        }

        // 2. Check NVM shared buffer (unflushed message)
        auto bufferedMessage = lookupInNVMBuffer(key);
        if (bufferedMessage.has_value()) {
            auto [op, val] = bufferedMessage.value();
            if (op == Operations::Insert || op == Operations::Update) {
                value = val;
                updateReadCache(key, value);
                return ErrorCode::Success;
            }
            if (op == Operations::Delete) {
                return ErrorCode::KeyDoesNotExist;
            }
        }

        // 3. Traverse the tree
        if (!root) return ErrorCode::KeyDoesNotExist;

        auto current = root;
        while (!current->isLeaf) {
            size_t i = std::upper_bound(current->keys.begin(),
                                        current->keys.end(),
                                        key) - current->keys.begin();
            if (i >= current->children.size())
                return ErrorCode::KeyDoesNotExist;

            current = current->children[i];
        }

        // 4. Look in the leaf node
        auto keyIt = std::find(current->keys.begin(), current->keys.end(), key);
        if (keyIt != current->keys.end()) {
            size_t index = std::distance(current->keys.begin(), keyIt);
            value = current->values[index];
            updateReadCache(key, value);
            return ErrorCode::Success;
        }
        std::cout << "[DEBUG] Failed to find key " << key << " in leaf with keys: ";
        for (auto k : current->keys) std::cout << k << " ";
        std::cout << "\n";


        return ErrorCode::KeyDoesNotExist;
    }


    // === RANGE QUERY ===
    std::vector<std::pair<KeyType, ValueType>> rangeQuery(KeyType low, KeyType high)
    {
        std::vector<std::pair<KeyType, ValueType>> result;
        auto current = root;

        // descend to first relevant leaf
        while (current && !current->isLeaf) {
            size_t i = std::upper_bound(current->keys.begin(),
                                        current->keys.end(), low)
                    - current->keys.begin();
            if (i >= current->children.size()) break;
            current = current->children[i];
        }

        // scan leaves and collect in-range keys
        while (current) {
            for (size_t i = 0; i < current->keys.size(); ++i) {
                KeyType k = current->keys[i];
                if (k > high) goto Done;
                if (k >= low) {
                    result.emplace_back(k, current->values[i]);
                }
            }

            // Move to next leaf naively
            auto parent = findParent(root, current);
            while (parent) {
                size_t index = std::find(parent->children.begin(), parent->children.end(), current)
                            - parent->children.begin();
                if (index + 1 < parent->children.size()) {
                    current = parent->children[index + 1];
                    while (!current->isLeaf)
                        current = current->children[0];
                    break;
                } else {
                    current = parent;
                    parent = findParent(root, parent);
                }
            }
            if (!parent) break;
        }

    Done:
        // overlay messages from NVM shared buffer
        if (pmemPoolHandle) {
            TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            auto* rootPtr = D_RO(root);

            for (int i = 0; i < rootPtr->messageCount; ++i) {
                auto* msg = D_RO(rootPtr->messages[i]);
                if (msg->key_size != sizeof(KeyType)) continue;

                KeyType k;
                std::memcpy(&k, msg->key_data, sizeof(KeyType));
                if (k < low || k > high) continue;

                Operations op = static_cast<Operations>(msg->opCode);
                ValueType v;
                std::memcpy(&v, msg->val_data, sizeof(ValueType));

                if (op == Operations::Insert || op == Operations::Update) {
                    auto it = std::find_if(result.begin(), result.end(),
                        [&](const auto& pair) { return pair.first == k; });
                    if (it != result.end()) {
                        it->second = v;  // update existing
                    } else {
                        result.emplace_back(k, v);  // new entry
                    }
                } else if (op == Operations::Delete) {
                    result.erase(std::remove_if(result.begin(), result.end(),
                        [&](const auto& pair) { return pair.first == k; }), result.end());
                }
            }
        }

        // sort and deduplicate (just in case)
        std::sort(result.begin(), result.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
        result.erase(std::unique(result.begin(), result.end(),
                [](const auto& a, const auto& b) { return a.first == b.first; }),
                result.end());

        return result;
    }


    // === INSERT OP ===
    ErrorCode insert(KeyType key, ValueType value)
    {
        if (isReplaying) return ErrorCode::Success; // skip actual mutation during replay

        logOperationBinary(Operations::Insert, key, value);

        if (!root)
        {
            root = std::make_shared<Node<KeyType, ValueType>>(true);
            root->keys.push_back(key);
            root->values.push_back(value);
            maybeCheckpoint();
            return ErrorCode::Success;
        }

        auto current = root;

        // If internal node
        if (!current->isLeaf)
        {
            auto bufferTarget = findBufferTarget(key);
            ErrorCode result = insertBuffered(bufferTarget, Operations::Insert, key, value);
            if (result != ErrorCode::Success) {
                return result;
            }
            maybeCheckpoint();
            return ErrorCode::Success;
        }
        else
        {
            // Leaf insertion
            auto it_keys = std::lower_bound(current->keys.begin(), current->keys.end(), key);
            size_t idx = std::distance(current->keys.begin(), it_keys);

            current->keys.insert(it_keys, key);
            current->values.insert(current->values.begin() + idx, value);

            // Split if overfull
            if (current->keys.size() >= m_nDegree) {
                auto parent = findParent(root, current);
                ErrorCode splitResult = splitLeaf(parent, current);
                if (splitResult != ErrorCode::Success) {
                    return splitResult;
                }
            }
            maybeCheckpoint();
            return ErrorCode::Success;
        }
    }

    // Insert op into buffer of an internal node
    ErrorCode insertBuffered(std::shared_ptr<Node<KeyType, ValueType>>, 
                         Operations op, KeyType key, ValueType value) 
    {
        // Insert into NVM shared buffer (may trigger flush and tree reorganization)
        ErrorCode res = insertToNVMSharedBuffer(op, key, value);
        if (res != ErrorCode::Success) return res;

        // Re-fetch buffer target using the updated tree structure
        auto node = findBufferTarget(key);

        // Track message-node mappings
        uint64_t node_id = static_cast<uint64_t>(getNodeKey(node));
        if (op != Operations::Delete) {
            addKeyToNodeMessageMap(node_id, key);
            insertToMessageToNodeMap(static_cast<uint64_t>(key), node_id);
        }


        // Frequency tracking for LFU flush
        KeyType nodeKey = getNodeKey(node);
        incrementNodeFrequency(static_cast<uint64_t>(nodeKey));
        return ErrorCode::Success;
    }

    // Flush the least-frequently-used node
    // ErrorCode flushLFUNode()
    // {
    //     TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
    //     auto* auxPtr = D_RW(auxRoot);

    //     if (auxPtr->nodeFrequencyCount == 0 || nodeMessageMap.empty())
    //         return ErrorCode::Success;

    //     // Find node_id with minimum frequency
    //     uint64_t minFreqNodeId = 0;
    //     int minFreq = INT32_MAX;

    //     for (int i = 0; i < auxPtr->nodeFrequencyCount; ++i) {
    //         auto* entry = D_RW(auxPtr->nodeFrequencyMap[i]);
    //         if (entry->frequency < minFreq) {
    //             minFreq = entry->frequency;
    //             minFreqNodeId = entry->node_id;
    //         }
    //     }

    //     // Find matching node in nodeMessageMap using node_id
    //     auto nodeIt = std::find_if(nodeMessageMap.begin(), nodeMessageMap.end(),
    //         [&](auto& pair) {
    //             auto node = pair.first;
    //             return !node->keys.empty() && static_cast<uint64_t>(node->keys[0]) == minFreqNodeId;
    //         });

    //     if (nodeIt == nodeMessageMap.end())
    //         return ErrorCode::Error;

    //     auto nodeToFlush = nodeIt->first;

    //     ErrorCode res = flushBuffer(nodeToFlush);
    //     return res;
    // }


    std::shared_ptr<Node<KeyType, ValueType>> findBufferTarget(const KeyType& key) {
        auto current = this->root;        
        while (current && !current->isLeaf) {
            size_t i = std::lower_bound(current->keys.begin(), current->keys.end(), key)
                     - current->keys.begin();
            if (i >= current->children.size()) break;
            auto next = current->children[i];
            if (next->isLeaf) return current; // buffer at current internal
            current = next;
        }
        return this->root;  // fallback
    }
    

    // Flush buffer messages for a node down to its children
    ErrorCode flushBuffer(std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        if (node->isLeaf) return ErrorCode::Success;

        uint64_t node_id = static_cast<uint64_t>(getNodeKey(node));
        auto keysToFlush = getBufferedKeysForNode(node_id);
        removeKeyFromNodeMessageMap(node_id);  // clear all keys at once if needed


        // Sort & unique keys
        std::sort(keysToFlush.begin(), keysToFlush.end());
        keysToFlush.erase(std::unique(keysToFlush.begin(), keysToFlush.end()), keysToFlush.end());
        for (const auto& key : keysToFlush)
        {
            // 1. Locate message in NVM shared buffer
            TOID(PMEMRoot) nvmRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
            auto* rootPtr = D_RW(nvmRoot);
            // auto* rootReadPtr = D_RO(nvmRoot);
            message* msgPtr = nullptr;

            for (int i = 0; i < rootPtr->messageCount; ++i) {
                auto* candidate = D_RW(rootPtr->messages[i]);
                if (candidate->key_size == sizeof(KeyType) &&
                    std::memcmp(candidate->key_data, &key, sizeof(KeyType)) == 0) {
                    msgPtr = candidate;
                    break;
                }
            }
            if (!msgPtr) continue;  // message not found (already flushed?)

            Operations opType = static_cast<Operations>(msgPtr->opCode);
            KeyType msgKey;
            ValueType msgVal;
            std::memcpy(&msgKey, msgPtr->key_data, sizeof(KeyType));
            std::memcpy(&msgVal, msgPtr->val_data, sizeof(ValueType));

            // 2. Route to correct child
            size_t idx = std::upper_bound(node->keys.begin(), node->keys.end(), msgKey)
                    - node->keys.begin();
            if (idx >= node->children.size()) continue;

            auto child = node->children[idx];

            if (!child->isLeaf) {
                auto bufferTarget = findBufferTarget(msgKey);
                insertBuffered(bufferTarget, opType, msgKey, msgVal);
            
                // Handle potential overflow of the internal node
                if (bufferTarget->keys.size() >= m_nDegree) {
                    auto parent = findParent(root, bufferTarget);
                    ErrorCode result = splitInternal(parent, bufferTarget);
                    if (result != ErrorCode::Success) return result;
                }
            }
             else {
                // Leaf: apply operation directly
                auto it = std::find(child->keys.begin(), child->keys.end(), msgKey);

                if (opType == Operations::Insert) {
                    if (it == child->keys.end()) {
                        auto insertIt = std::lower_bound(child->keys.begin(), child->keys.end(), msgKey);
                        size_t pos = std::distance(child->keys.begin(), insertIt);
                        child->keys.insert(insertIt, msgKey);
                        child->values.insert(child->values.begin() + pos, msgVal);
                        if (child->keys.size() >= m_nDegree) 
                        {
                            ErrorCode result = splitLeaf(node, child);
                            if (result != ErrorCode::Success) return result;
                            node = findParent(root, child);
                            //node = root;
                        }
                    }

                } else if (opType == Operations::Update) {
                    if (it != child->keys.end()) {
                        size_t pos = std::distance(child->keys.begin(), it);
                        child->values[pos] = msgVal;
                    }
                } else if (opType == Operations::Delete) {
                    if (it != child->keys.end()) {
                        size_t pos = std::distance(child->keys.begin(), it);
                        child->keys.erase(it);
                        child->values.erase(child->values.begin() + pos);
                        // Handle underflow if necessary
                        if (child->keys.size() < (m_nDegree / 2)) 
                        {
                            ErrorCode result = handleUnderflow(node, child);
                            if (result != ErrorCode::Success) return result;
                        }
                    }
                }
            }

            // 3. Clean up buffer references
            removeKeyFromMessageToNodeMap(static_cast<uint64_t>(key));
            removeKeyFromNodeMessageMap(node_id, key);

            // 4. Remove from NVM shared buffer (compact array)
            for (int i = 0; i < rootPtr->messageCount; ++i) {
                auto* candidate = D_RW(rootPtr->messages[i]);
                if (candidate == msgPtr) {
                    // Free memory
                    pmemobj_free(&rootPtr->messages[i].oid);

                    // Shift all later entries
                    for (int j = i + 1; j < rootPtr->messageCount; ++j) {
                        rootPtr->messages[j - 1] = rootPtr->messages[j];
                    }
                    rootPtr->messageCount--;
                    pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(PMEMRoot));
                    break;
                }
            }
        }

        return ErrorCode::Success;
    }
    
    // === SPLIT LEAF ===
    ErrorCode splitLeaf(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> leaf)
    {
        auto sibling = std::make_shared<Node<KeyType, ValueType>>(true);

        int n = static_cast<int>(leaf->keys.size());
        int mid = n / 2; // pivot index for "classic" split

        // Pivot key is the "middle" key
        KeyType pivotKey = leaf->keys[mid];

        // Sibling gets everything after the pivot
        sibling->keys.assign(leaf->keys.begin() + mid , leaf->keys.end());
        sibling->values.assign(leaf->values.begin() + mid , leaf->values.end());

        // Leaf keeps [0..mid-1]
        leaf->keys.resize(mid);
        leaf->values.resize(mid);

        // Reassign any buffered messages that belong >= pivotKey to sibling
        {
            uint64_t leaf_id = static_cast<uint64_t>(getNodeKey(leaf));
            uint64_t sibling_id = static_cast<uint64_t>(getNodeKey(sibling));

            auto keys = getBufferedKeysForNode(leaf_id);
            for (auto& k : keys) {
                if (k >= pivotKey) {
                    moveBufferedKeyBetweenNodes(leaf_id, sibling_id, k);
                    insertToMessageToNodeMap(static_cast<uint64_t>(k), sibling_id);
                }
            }
        }

        if (!parent) {
            // Create new root
            auto newRoot = std::make_shared<Node<KeyType, ValueType>>(false);
            newRoot->keys.push_back(pivotKey);
            newRoot->children.push_back(leaf);
            newRoot->children.push_back(sibling);
            root = newRoot;
        }
        else {
            // Insert pivotKey in sorted order into parent
            auto insertIt = std::lower_bound(parent->keys.begin(), parent->keys.end(), pivotKey);
            size_t pos = std::distance(parent->keys.begin(), insertIt);

            parent->keys.insert(insertIt, pivotKey);
            parent->children.insert(parent->children.begin() + pos + 1, sibling);

            // Check if parent overfull
            if (parent->keys.size() >= m_nDegree) {
                auto grandparent = findParent(root, parent);
                return splitInternal(grandparent, parent);
            }
        }
        return ErrorCode::Success;
    }

    // === SPLIT INTERNAL ===
    ErrorCode splitInternal(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> internal)
    {
        auto sibling = std::make_shared<Node<KeyType, ValueType>>(false);

        int mid = static_cast<int>(internal->keys.size()) / 2;
        KeyType pivotKey = internal->keys[mid];

        sibling->keys.assign(internal->keys.begin() + mid + 1, internal->keys.end());
        sibling->children.assign(internal->children.begin() + mid + 1, internal->children.end());

        internal->keys.resize(mid);
        internal->children.resize(mid + 1);

        if (!parent) {
            // New root
            auto newRoot = std::make_shared<Node<KeyType, ValueType>>(false);
            newRoot->keys.push_back(pivotKey);
            newRoot->children.push_back(internal);
            newRoot->children.push_back(sibling);
            root = newRoot;
        }
        else {
            // Insert pivotKey into parent
            auto insertIt = std::lower_bound(parent->keys.begin(), parent->keys.end(), pivotKey);
            size_t pos = std::distance(parent->keys.begin(), insertIt);
            parent->keys.insert(insertIt, pivotKey);
            parent->children.insert(parent->children.begin() + pos + 1, sibling);

            // If parent is overfull, split again
            if (parent->keys.size() >= m_nDegree) {
                auto grandparent = findParent(root, parent);
                return splitInternal(grandparent, parent);
            }
        }

        // Reassign buffered messages from 'internal' to 'sibling' if >= pivotKey
        uint64_t from_id = static_cast<uint64_t>(getNodeKey(internal));
        uint64_t to_id = static_cast<uint64_t>(getNodeKey(sibling));

        auto keys = getBufferedKeysForNode(from_id);
        for (auto& k : keys) {
            if (k >= pivotKey) {
                moveBufferedKeyBetweenNodes(from_id, to_id, k);
                insertToMessageToNodeMap(static_cast<uint64_t>(k), to_id);
                std::cout << "[DEBUG] Reassigned key " << k << " to sibling during internal split\n";
            }
        }
        return ErrorCode::Success;
    }

    // === Helper to find parent of a node ===
    std::shared_ptr<Node<KeyType, ValueType>> findParent(std::shared_ptr<Node<KeyType, ValueType>> current,
        std::shared_ptr<Node<KeyType, ValueType>> child)
    {
        if (!current || current->isLeaf) return nullptr;
        for (auto& c : current->children) {
            if (c == child) return current;
        }
        // Recurse
        for (auto& c : current->children) {
            auto p = findParent(c, child);
            if (p) return p;
        }
        return nullptr;
    }

    // === UPSERT OP === (Insert if absent, else Update)
    ErrorCode upsert(KeyType key, ValueType value)
    {
        logOperationBinary(Operations::Upsert, key, value);

        if (!root) {
            root = std::make_shared<Node<KeyType, ValueType>>(true);
            root->keys.push_back(key);
            root->values.push_back(value);
            maybeCheckpoint();
            return ErrorCode::Success;
        }

        auto current = root;
        if (!current->isLeaf)
        {
            // Buffer it
            ErrorCode res = insertBuffered(current, Operations::Insert, key, value);
            if (res != ErrorCode::Success) return res;
            maybeCheckpoint();
            return ErrorCode::Success;
        }
        else
        {
            // Leaf: check if key exists
            auto it = std::find(current->keys.begin(), current->keys.end(), key);
            if (it != current->keys.end()) {
                // Update
                size_t idx = std::distance(current->keys.begin(), it);
                current->values[idx] = value;
            }
            else {
                // Insert
                auto itK = std::lower_bound(current->keys.begin(), current->keys.end(), key);
                size_t pos = std::distance(current->keys.begin(), itK);
                current->keys.insert(itK, key);
                current->values.insert(current->values.begin() + pos, value);

                // Split if needed
                if (current->keys.size() >= m_nDegree) {
                    auto parent = findParent(root, current);
                    splitLeaf(parent, current);
                }
            }
            maybeCheckpoint();
            return ErrorCode::Success;
        }
    }

    // === Debug Print Methods ===
    void display(std::shared_ptr<Node<KeyType, ValueType>> node, int level)
    {
        if (!node) return;

        cout << std::string(level * 2, ' ') << "[";
        for (size_t i = 0; i < node->keys.size(); i++)
        {
            if (!node->isLeaf) {
                cout << node->keys[i] << " ";
            }
            else {
                cout << node->keys[i] << ":" << node->values[i] << " ";
            }
        }
        cout << "]\n";

        for (auto& child : node->children) {
            display(child, level + 1);
        }
    }

    
    void printNVMMessageToNodeMap() const {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RO(auxRoot);

        std::cout << "\n--- [DEBUG] Persistent messageToNodeMap ---\n";
        if (auxPtr->messageToNodeMapCount == 0) {
            std::cout << "(empty)\n";
            return;
        }

        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i) {
            auto* entry = D_RO(auxPtr->messageToNodeMap[i]);
            std::cout << "Key[" << entry->key << "] => NodeID[" << entry->node_id << "]\n";
        }
    }


    void printReadCache() const {
        std::cout << "\n--- [DEBUG] DRAM Read Cache ---\n";
        if (readCache.empty()) {
            std::cout << "(empty)\n";
            return;
        }
        for (auto& [k, v] : readCache) {
            std::cout << "Key: " << k << ", Value: " << v << "\n";
        }
    }

    void printNVMSharedBuffer() {
        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* rootPtr = D_RO(root);
        std::cout << "\n[NVM BUFFER DEBUG] Current entries: " << rootPtr->messageCount << "\n";
        for (int i = 0; i < rootPtr->messageCount; ++i) {
            auto* msg = D_RO(rootPtr->messages[i]);
            std::cout << decodeMessageEntry(i, *msg) << "\n";
        }
    }

    void printNVMNodeFrequency() {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);        auto* auxPtr = D_RO(auxRoot);
    
        std::cout << "\n--- [DEBUG] Persistent Node Frequency ---\n";
        for (int i = 0; i < auxPtr->nodeFrequencyCount; ++i) {
            auto* entry = D_RO(auxPtr->nodeFrequencyMap[i]);
            std::cout << "NodeID[" << entry->node_id << "] => freq=" << entry->frequency << "\n";
        }
    }

    void printNVMNodeMessageMap() const {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RO(auxRoot);
    
        std::cout << "\n--- [DEBUG] Persistent nodeMessageMap ---\n";
        if (auxPtr->nodeMessageMapCount == 0) {
            std::cout << "(empty)\n";
            return;
        }
    
        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i) {
            auto* entry = D_RO(auxPtr->nodeMessageMap[i]);
            std::cout << "NodeID[" << entry->node_id << "] => buffered keys: ";
            for (int j = 0; j < entry->key_list.count; ++j) {
                std::cout << entry->key_list.keys[j] << " ";
            }
            std::cout << "\n";
        }
    }
    
    
    
    std::string decodeMessageEntry(int index, const message& msg) {
        std::stringstream ss;
        ss << "[" << index << "] ";
    
        switch (msg.opCode) {
            case 0: ss << "Insert"; break;
            case 1: ss << "Search"; break;
            case 2: ss << "Update"; break;
            case 3: ss << "Delete"; break;
            case 4: ss << "Upsert"; break;
            default: ss << "Unknown(" << static_cast<int>(msg.opCode) << ")";
        }
    
        ss << " | Key: ";
        if (msg.key_size == 4) {
            int32_t key;
            std::memcpy(&key, msg.key_data, 4);
            ss << key;
        } else if (msg.key_size == 8) {
            int64_t key;
            std::memcpy(&key, msg.key_data, 8);
            ss << key;
        } else if (msg.key_size == 16) {
            for (int i = 0; i < 16; ++i)
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(msg.key_data[i]));
        } else {
            ss << "(unknown/" << msg.key_size << " bytes)";
        }
    
        ss << " | Value: ";
        if (msg.val_size == 4) {
            int32_t val;
            std::memcpy(&val, msg.val_data, 4);
            ss << val;
        } else if (msg.val_size == 8) {
            int64_t val;
            std::memcpy(&val, msg.val_data, 8);
            ss << val;
        } else if (msg.val_size == 16) {
            for (int i = 0; i < 16; ++i)
                ss << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<int>(static_cast<unsigned char>(msg.val_data[i]));
        } else {
            ss << "(unknown/" << msg.val_size << " bytes)";
        }
    
        return ss.str();
    }

    std::optional<std::tuple<Operations, ValueType>> lookupInNVMBuffer(const KeyType& key)
    {
        if (!pmemPoolHandle) return std::nullopt;

        TOID(PMEMRoot) root = POBJ_ROOT(pmemPoolHandle, PMEMRoot);

        auto* rootPtr = D_RO(root);

        for (int i = 0; i < rootPtr->messageCount; ++i) {
            auto* msg = D_RO(rootPtr->messages[i]);
            if (msg->key_size != sizeof(KeyType)) continue;

            if (std::memcmp(msg->key_data, &key, sizeof(KeyType)) == 0) {
                Operations op = static_cast<Operations>(msg->opCode);
                ValueType val;
                std::memcpy(&val, msg->val_data, sizeof(ValueType));
                return std::make_tuple(op, val);
            }
        }

        return std::nullopt;  // Not found
    }

    ErrorCode flushMostBufferedNode()
    {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RO(auxRoot);

        if (auxPtr->nodeMessageMapCount == 0)
            return ErrorCode::Success;

        // Find node with the largest number of buffered keys
        int maxIndex = -1;
        int maxKeys = -1;

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i) {
            auto* entry = D_RO(auxPtr->nodeMessageMap[i]);
            if (entry->key_list.count > maxKeys) {
                maxKeys = entry->key_list.count;
                maxIndex = i;
            }
        }

        if (maxIndex == -1) {
            std::cerr << "[FLUSH] No node with buffered messages.\n";
            return ErrorCode::Error;
        }

        auto* entry = D_RO(auxPtr->nodeMessageMap[maxIndex]);
        uint64_t node_id = entry->node_id;

        // Match node_id to actual in-memory node
        std::shared_ptr<Node<KeyType, ValueType>> target = nullptr;
        std::function<void(std::shared_ptr<Node<KeyType, ValueType>>)> dfs = [&](std::shared_ptr<Node<KeyType, ValueType>> node) {
            if (!node || node->keys.empty()) return;
            if (static_cast<uint64_t>(node->keys[0]) == node_id) {
                target = node;
                return;
            }
            for (auto& child : node->children) {
                dfs(child);
                if (target) return;
            }
        };

        dfs(root);

        if (!target) {
            std::cerr << "[FLUSH] Failed to locate in-memory node for node_id " << node_id << "\n";
            return ErrorCode::Error;
        }
        return flushBuffer(target);
    }

    std::string getPlatformPath(const std::string& filename) {
        #ifdef _WIN32
            return "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\" + filename;
        #else
            return "/home/ademzarrouki/Desktop/Benchmark/" + filename;
        #endif
    }

    // === Persistent nodeMessageMap Helpers ===
    void addKeyToNodeMessageMap(uint64_t node_id, KeyType key) {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i) {
            auto* entry = D_RW(auxPtr->nodeMessageMap[i]);
            if (entry->node_id == node_id) {
                for (int j = 0; j < entry->key_list.count; ++j) {
                    if (entry->key_list.keys[j] == key) return; // already present
                }
                if (entry->key_list.count < MAX_KEYS_PER_NODE) {
                    entry->key_list.keys[entry->key_list.count++] = key;
                    pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeMessageEntry));
                }
                return;
            }
        }

        // New entry
        if (auxPtr->nodeMessageMapCount < MAX_NODES) {
            TOID(NodeMessageEntry) newEntry;
            if (pmemobj_alloc(pmemPoolHandle, &newEntry.oid, sizeof(NodeMessageEntry), 0, nullptr, nullptr) == 0) {
                auto* newPtr = D_RW(newEntry);
                newPtr->node_id = node_id;
                newPtr->key_list.count = 1;
                newPtr->key_list.keys[0] = key;
                pmemobj_persist(pmemPoolHandle, newPtr, sizeof(NodeMessageEntry));

                auxPtr->nodeMessageMap[auxPtr->nodeMessageMapCount++] = newEntry;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot)); 
            }
        }
    }

    std::vector<KeyType> getBufferedKeysForNode(uint64_t node_id) {
        std::vector<KeyType> result;
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RO(auxRoot);

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i) {
            auto* entry = D_RO(auxPtr->nodeMessageMap[i]);
            if (entry->node_id == node_id) {
                for (int j = 0; j < entry->key_list.count; ++j) {
                    result.push_back(static_cast<KeyType>(entry->key_list.keys[j]));
                }
                break;
            }
        }
        return result;
    }

    void removeKeyFromNodeMessageMap(uint64_t node_id, KeyType key) {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->nodeMessageMapCount; ++i) {
            auto* entry = D_RW(auxPtr->nodeMessageMap[i]);
            if (entry->node_id == node_id) {
                auto& list = entry->key_list;
                int j = 0;
                while (j < list.count) {
                    if (list.keys[j] == key) {
                        for (int k = j + 1; k < list.count; ++k) {
                            list.keys[k - 1] = list.keys[k];
                        }
                        list.count--;
                        pmemobj_persist(pmemPoolHandle, entry, sizeof(NodeMessageEntry));
                        break;
                    }
                    ++j;
                }
                return;
            }
        }
    }

    void removeKeyFromNodeMessageMap(uint64_t node_id) {
        auto keys = getBufferedKeysForNode(node_id);
        for (const auto& k : keys) {
            removeKeyFromNodeMessageMap(node_id, k);
        }
    }

    void moveBufferedKeyBetweenNodes(uint64_t from_id, uint64_t to_id, KeyType key) {
        removeKeyFromNodeMessageMap(from_id, key);
        addKeyToNodeMessageMap(to_id, key);
    }

    void insertToMessageToNodeMap(uint64_t key, uint64_t node_id) {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RW(auxRoot);

        // Overwrite if key already exists
        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i) {
            auto* entry = D_RW(auxPtr->messageToNodeMap[i]);
            if (entry->key == key) {
                entry->node_id = node_id;
                pmemobj_persist(pmemPoolHandle, entry, sizeof(MessageToNodeEntry));
                return;
            }
        }

        // Insert new entry
        if (auxPtr->messageToNodeMapCount < MAX_MESSAGES) {
            TOID(MessageToNodeEntry) newEntry;
            if (pmemobj_alloc(pmemPoolHandle, &newEntry.oid, sizeof(MessageToNodeEntry), 0, nullptr, nullptr) == 0) {
                D_RW(newEntry)->key = key;
                D_RW(newEntry)->node_id = node_id;
                pmemobj_persist(pmemPoolHandle, D_RW(newEntry), sizeof(MessageToNodeEntry));
                auxPtr->messageToNodeMap[auxPtr->messageToNodeMapCount++] = newEntry;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot)); 
            }
        }
    }

    std::optional<uint64_t> lookupNodeIdForKey(uint64_t key) {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RO(auxRoot);

        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i) {
            auto* entry = D_RO(auxPtr->messageToNodeMap[i]);
            if (entry->key == key) {
                return entry->node_id;
            }
        }

        return std::nullopt;
    }

    void removeKeyFromMessageToNodeMap(uint64_t key) {
        TOID(PMEMRoot) auxRoot = POBJ_ROOT(pmemPoolHandle, PMEMRoot);
        auto* auxPtr = D_RW(auxRoot);

        for (int i = 0; i < auxPtr->messageToNodeMapCount; ++i) {
            auto* entry = D_RW(auxPtr->messageToNodeMap[i]);
            if (entry->key == key) {
                // Free and shift
                pmemobj_free(&auxPtr->messageToNodeMap[i].oid);
                for (int j = i + 1; j < auxPtr->messageToNodeMapCount; ++j) {
                    auxPtr->messageToNodeMap[j - 1] = auxPtr->messageToNodeMap[j];
                }
                auxPtr->messageToNodeMapCount--;
                pmemobj_persist(pmemPoolHandle, auxPtr, sizeof(PMEMRoot)); 
                return;
            }
        }
    }     
};