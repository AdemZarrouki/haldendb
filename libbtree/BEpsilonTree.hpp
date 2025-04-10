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
    PMEMobjpool* pmemPoolHandle = nullptr;  // NVM pool for shared buffer

    uint32_t m_nDegree;
    std::string logFilename = "/home/ademzarrouki/Desktop/Benchmark/nvm_tree_test1.log";
    int opCounter = 0;
    int checkpointFrequency = 5;
    bool isReplaying = false;

    std::unordered_map<std::shared_ptr<Node<KeyType, ValueType>>, std::vector<KeyType>> nodeMessageMap;
    std::unordered_map<KeyType, std::shared_ptr<Node<KeyType, ValueType>>> messageToNodeMap;
    std::unordered_map<KeyType, int> nodeFrequency;

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
        const std::string pmemPath = "/home/ademzarrouki/Desktop/Benchmark/shared_buffer_pool.pmem";

        std::cout << "[DEBUG] Trying to open PMEM pool at: " << pmemPath << "\n";

        pmemPoolHandle = pmemobj_open(pmemPath.c_str(), LAYOUT_NAME);
        if (!pmemPoolHandle) {
            std::cout << "[DEBUG] Pool not found. Creating new PMEM pool...\n";
            pmemPoolHandle = pmemobj_create(pmemPath.c_str(), LAYOUT_NAME,
                                            64 * 1024 * 1024, 0666);  // Try 64MB for now
            if (!pmemPoolHandle) {
                std::cerr << "[ERROR] Failed to create PMEM pool! Exiting.\n";
                perror("pmemobj_create");
                exit(1);
            } else {
                std::cout << "[NVM] New PMEM pool created successfully.\n";
            }
        } else {
            std::cout << "[NVM] Existing PMEM pool opened successfully.\n";
        }

        // === Replay Binary WAL if present ===
        std::ifstream walBin("/home/ademzarrouki/Desktop/Benchmark/nvm_tree_bin.wal", std::ios::binary);
        if (walBin.is_open()) {
            isReplaying = true;
            while (!walBin.eof()) {
                uint8_t opCode;
                KeyType key;
                ValueType value;

                walBin.read(reinterpret_cast<char*>(&opCode), sizeof(opCode));
                if (walBin.eof()) break;  // avoid partial read

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
                    std::cerr << "[WARN] Unsupported op in binary WAL: " << static_cast<int>(op) << "\n";
                    break;
                }
            }
            walBin.close();
        }
        isReplaying = false;
    }

    ~BEpsilonTree()
    {
        // Save final tree
        saveTreeToFile(root, "/home/ademzarrouki/Desktop/Benchmark/tree_data.bin");

        // Checkpoint if we have outstanding ops
        if (opCounter > 0) {
            checkpoint();
        }

        // Backup and clear binary WAL
        const std::string binWal = "/home/ademzarrouki/Desktop/Benchmark/nvm_tree_bin.wal";
        const std::string bakName = "/home/ademzarrouki/Desktop/Benchmark/wal_bin_backup" + timestampename() + ".bak";

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
    ErrorCode insertToNVMSharedBuffer(Operations op, const KeyType& key, const ValueType& value = ValueType{}) 
    {
        if (!pmemPoolHandle) return ErrorCode::Error;

        TOID(SharedBufferRoot) root = POBJ_ROOT(pmemPoolHandle, SharedBufferRoot);
        auto* rootPtr = D_RW(root);

        // Search for existing message on the same key
        int existingIndex = -1;
        for (int i = 0; i < rootPtr->count; ++i) {
            message* msgPtr = D_RW(rootPtr->messages[i]);
            if (msgPtr->key_size == sizeof(KeyType) &&
                std::memcmp(msgPtr->key_data, &key, sizeof(KeyType)) == 0) {
                existingIndex = i;
                break;
            }
        }

        // Coalescing logic
        if (existingIndex != -1) {
            message* msgPtr = D_RW(rootPtr->messages[existingIndex]);
            Operations prevOp = static_cast<Operations>(msgPtr->opCode);

            switch (prevOp) {
                case Operations::Insert:
                    if (op == Operations::Update) {
                        msgPtr->opCode = static_cast<uint8_t>(Operations::Insert);
                        std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
                        pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
                        return ErrorCode::Success;
                    } else if (op == Operations::Delete) {
                        for (int j = existingIndex + 1; j < rootPtr->count; ++j)
                            rootPtr->messages[j - 1] = rootPtr->messages[j];
                        rootPtr->count--;
                        pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(SharedBufferRoot));
                        return ErrorCode::Success;
                    }
                    break;
                case Operations::Update:
                    if (op == Operations::Update) {
                        std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
                        pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
                        return ErrorCode::Success;
                    } else if (op == Operations::Delete) {
                        msgPtr->opCode = static_cast<uint8_t>(Operations::Delete);
                        pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
                        return ErrorCode::Success;
                    }
                    break;
                case Operations::Delete:
                    if (op == Operations::Insert) {
                        msgPtr->opCode = static_cast<uint8_t>(Operations::Insert);
                        std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
                        pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
                        return ErrorCode::Success;
                    }
                    break;
                default:
                    break;
            }

            // Fallback: overwrite message
            msgPtr->opCode = static_cast<uint8_t>(op);
            std::memcpy(msgPtr->val_data, &value, sizeof(ValueType));
            pmemobj_persist(pmemPoolHandle, msgPtr, sizeof(message));
            return ErrorCode::Success;
        }

        // Flush if full
        while (rootPtr->count >= MAX_NVM_MESSAGES) {
            std::cout << "[NVM] Shared buffer full, flushing...\n";
            auto flushResult = flushMostBufferedNode();
            if (flushResult != ErrorCode::Success) {
                std::cerr << "[ERROR] Failed to flush any node. Shared buffer stuck.\n";
                return ErrorCode::Error;
            }

            // Recompute rootPtr in case root was replaced during flush/split
            root = POBJ_ROOT(pmemPoolHandle, SharedBufferRoot);
            rootPtr = D_RW(root);
        }

        // Insert new message
        TOID(message) msg;
        if (pmemobj_alloc(pmemPoolHandle, &msg.oid, sizeof(message), 0, nullptr, nullptr) != 0) {
            std::cerr << "[NVM] Failed to allocate message in PMEM!\n";
            return ErrorCode::Error;
        }

        D_RW(msg)->opCode = static_cast<uint8_t>(op);
        D_RW(msg)->key_size = sizeof(KeyType);
        D_RW(msg)->val_size = sizeof(ValueType);
        std::memcpy(D_RW(msg)->key_data, &key, sizeof(KeyType));
        std::memcpy(D_RW(msg)->val_data, &value, sizeof(ValueType));
        pmemobj_persist(pmemPoolHandle, D_RW(msg), sizeof(message));

        rootPtr->messages[rootPtr->count++] = msg;
        pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(SharedBufferRoot));

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
        saveTreeToFile(root, "/home/ademzarrouki/Desktop/Benchmark/tree_data.bin");

        std::string backupFilename = "/home/ademzarrouki/Desktop/Benchmark/wal_backup" + timestampename() + ".bak";

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

        std::cout << "[CHECKPOINT] Tree saved, WAL cleared, backup created: " << backupFilename << "\n";
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
        std::ofstream log("/home/ademzarrouki/Desktop/Benchmark/nvm_tree_bin.wal",
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
        auto it = nodeMessageMap.find(right);
        if (it != nodeMessageMap.end()) {
            for (auto& k : it->second) {
                nodeMessageMap[left].push_back(k);
                messageToNodeMap[k] = left;
            }
            nodeMessageMap.erase(it);
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
            TOID(SharedBufferRoot) root = POBJ_ROOT(pmemPoolHandle, SharedBufferRoot);
            auto* rootPtr = D_RO(root);

            for (int i = 0; i < rootPtr->count; ++i) {
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
        auto& keyList = nodeMessageMap[node];
        if (std::find(keyList.begin(), keyList.end(), key) == keyList.end()) {
            keyList.push_back(key);
        }

        messageToNodeMap[key] = node;

        // Frequency tracking for LFU flush
        KeyType nodeKey = getNodeKey(node);
        nodeFrequency[nodeKey]++;

        std::cout << "[DEBUG] insertBuffered(): Key " << key
                << " buffered into node with keys: ";
        for (auto k : node->keys) std::cout << k << " ";
        std::cout << "\n";

        return ErrorCode::Success;
    }

    // Flush the least-frequently-used node
    ErrorCode flushLFUNode()
    {
        if (nodeMessageMap.empty()) return ErrorCode::Success;

        // 1) find node with minimal frequency
        auto minIt = std::min_element(
            nodeFrequency.begin(), nodeFrequency.end(),
            [](auto& a, auto& b) { return a.second < b.second; }
        );
        if (minIt == nodeFrequency.end())
            return ErrorCode::Error;

        KeyType lfuKey = minIt->first;

        // 2) find matching node in nodeMessageMap
        auto nodeIt = std::find_if(nodeMessageMap.begin(), nodeMessageMap.end(),
            [&](auto& pair) {
                auto candidateNode = pair.first;
                if (candidateNode->keys.empty()) return false;
                // match by the first key in that node
                return (candidateNode->keys[0] == lfuKey);
            }
        );

        if (nodeIt == nodeMessageMap.end())
            return ErrorCode::Error;

        auto nodeToFlush = nodeIt->first;

        // 3) Flush
        ErrorCode res = flushBuffer(nodeToFlush);
        if (res == ErrorCode::Success) {
            nodeFrequency.erase(lfuKey);
        }

        return res;
    }

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
        // std::cout << "[DEBUG] findBufferTarget(): Key " << key
        //   << " -> target node with keys: ";
        // for (auto k : current->keys) std::cout << k << " ";
        // std::cout << "\n";

        return this->root;  // fallback
    }
    

    // Flush buffer messages for a node down to its children
    ErrorCode flushBuffer(std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        if (node->isLeaf) return ErrorCode::Success;

        auto it = nodeMessageMap.find(node);
        if (it == nodeMessageMap.end())
            return ErrorCode::Success;

        auto keysToFlush = it->second;
        nodeMessageMap.erase(it);

        // Sort & unique keys
        std::sort(keysToFlush.begin(), keysToFlush.end());
        keysToFlush.erase(std::unique(keysToFlush.begin(), keysToFlush.end()), keysToFlush.end());
        for (const auto& key : keysToFlush)
        {
            // 1. Locate message in NVM shared buffer
            TOID(SharedBufferRoot) nvmRoot = POBJ_ROOT(pmemPoolHandle, SharedBufferRoot);
            auto* rootPtr = D_RW(nvmRoot);
            message* msgPtr = nullptr;

            for (int i = 0; i < rootPtr->count; ++i) {
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
            auto msgNodeIt = messageToNodeMap.find(key);
            if (msgNodeIt != messageToNodeMap.end())
                messageToNodeMap.erase(msgNodeIt);

            auto& keyVec = nodeMessageMap[node];
            keyVec.erase(std::remove(keyVec.begin(), keyVec.end(), key), keyVec.end());

            // 4. Remove from NVM shared buffer (compact array)
            for (int i = 0; i < rootPtr->count; ++i) {
                auto* candidate = D_RW(rootPtr->messages[i]);
                if (candidate == msgPtr) {
                    // Free memory
                    pmemobj_free(&rootPtr->messages[i].oid);

                    // Shift all later entries
                    for (int j = i + 1; j < rootPtr->count; ++j) {
                        rootPtr->messages[j - 1] = rootPtr->messages[j];
                    }
                    rootPtr->count--;
                    pmemobj_persist(pmemPoolHandle, rootPtr, sizeof(SharedBufferRoot));
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
            auto itMap = nodeMessageMap.find(leaf);
            if (itMap != nodeMessageMap.end()) {
                // gather keys that must move
                std::vector<KeyType> moving;
                for (auto& k : itMap->second) {
                    if (k >= pivotKey) {
                        moving.push_back(k);
                    }
                }
                // remove from leaf's list, add to sibling
                for (auto& k : moving) {
                    auto& refList = nodeMessageMap[leaf];
                    refList.erase(std::remove(refList.begin(), refList.end(), k), refList.end());
                    nodeMessageMap[sibling].push_back(k);
                    messageToNodeMap[k] = sibling;
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
        auto itMap = nodeMessageMap.find(internal);
        if (itMap != nodeMessageMap.end()) {
            std::vector<KeyType> moving;
            for (auto& k : itMap->second) {
                if (k >= pivotKey) {
                    moving.push_back(k);
                }
            }
            for (auto& k : moving) {
                auto& oldList = nodeMessageMap[internal];
                oldList.erase(std::remove(oldList.begin(), oldList.end(), k), oldList.end());

                nodeMessageMap[sibling].push_back(k);
                messageToNodeMap[k] = sibling;

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

    void printNodeMessageMap() const {
        std::cout << "\n--- [DEBUG] nodeMessageMap Contents ---\n";
        if (nodeMessageMap.empty()) {
            std::cout << "(empty)\n";
            return;
        }
        for (auto& [node, keys] : nodeMessageMap) {
            std::cout << "Node(";
            for (auto& nk : node->keys) {
                std::cout << nk << " ";
            }
            std::cout << ") => buffered keys: ";
            for (auto& kk : keys) {
                std::cout << kk << " ";
            }
            std::cout << "\n";
        }
    }

    void printMessageToNodeMap() const {
        std::cout << "\n--- [DEBUG] messageToNodeMap Contents ---\n";
        if (messageToNodeMap.empty()) {
            std::cout << "(empty)\n";
            return;
        }
        for (auto& [k, n] : messageToNodeMap) {
            if (n) {
                std::cout << "Key " << k << " belongs to Node(";
                for (auto& x : n->keys) std::cout << x << " ";
                std::cout << ")\n";
            }
        }
    }

    void printNodeFrequency() const {
        std::cout << "\n--- [DEBUG] Node Frequency ---\n";
        if (nodeFrequency.empty()) {
            std::cout << "(empty)\n";
            return;
        }
        for (auto& [k, freq] : nodeFrequency) {
            std::cout << "NodeKey[" << k << "] => freq=" << freq << "\n";
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
        TOID(SharedBufferRoot) root = POBJ_ROOT(pmemPoolHandle, SharedBufferRoot);
        auto* rootPtr = D_RO(root);
        std::cout << "\n[NVM BUFFER DEBUG] Current entries: " << rootPtr->count << "\n";
        for (int i = 0; i < rootPtr->count; ++i) {
            auto* msg = D_RO(rootPtr->messages[i]);
            std::cout << decodeMessageEntry(i, *msg) << "\n";
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

        TOID(SharedBufferRoot) root = POBJ_ROOT(pmemPoolHandle, SharedBufferRoot);
        auto* rootPtr = D_RO(root);

        for (int i = 0; i < rootPtr->count; ++i) {
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
        if (nodeMessageMap.empty()) return ErrorCode::Success;

        auto maxIt = std::max_element(
            nodeMessageMap.begin(), nodeMessageMap.end(),
            [](const auto& a, const auto& b) {
                return a.second.size() < b.second.size();
            }
        );

        if (maxIt == nodeMessageMap.end()) {
            std::cerr << "[FLUSH] No node to flush.\n";
            return ErrorCode::Error;
        }

        auto nodeToFlush = maxIt->first;
        std::cout << "[FLUSH] Flushing node with " << maxIt->second.size()
                << " messages (first key: " << getNodeKey(nodeToFlush) << ")\n";

        ErrorCode res = flushBuffer(nodeToFlush);
        return res;
    }

};
