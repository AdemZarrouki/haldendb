#pragma once
#include "Node.hpp"
#include "ErrorCodes.h"
#include "Operations.h"
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

using namespace std;



template <typename KeyType, typename ValueType>
class BEpsilonTree
{
public:
    std::shared_ptr<Node<KeyType, ValueType>> root;
    uint32_t m_nDegree;
    uint32_t maxBufferSize;
    std::string logFilename = "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\nvm_tree_test1.log";
    int opCounter = 0;
    int checkpointFrequency = 5;
    bool isReplaying = false;

    // shared buffer
    std::unordered_map<KeyType, std::tuple<Operations, KeyType, ValueType>> sharedBuffer;
    std::unordered_map<std::shared_ptr<Node<KeyType, ValueType>>, std::vector<KeyType>> nodeMessageMap;
    std::unordered_map<KeyType, std::shared_ptr<Node<KeyType, ValueType>>> messageToNodeMap;
    std::unordered_map<KeyType, int> nodeFrequency;

    // DRAM read buffer
    std::unordered_map<KeyType, ValueType> readCache;
    std::list<KeyType> lruList;
    size_t maxReadCacheSize = 100;  // to be set


    BEpsilonTree(int m_nDegree, int maxBufferSize, const std::string& filename, int checkpointFrequency = -1)
    {
        this->m_nDegree = m_nDegree;
        this->maxBufferSize = maxBufferSize;
        if (checkpointFrequency > 0)
            this->checkpointFrequency = checkpointFrequency;

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

        // === WAL REPLAY GOES HERE ===
        std::ifstream walBin("C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\nvm_tree_bin.wal", std::ios::binary);
        if (walBin.is_open()) {
            isReplaying = true;
            while (!walBin.eof()) {
                uint8_t opCode;
                KeyType key;
                ValueType value;

                walBin.read(reinterpret_cast<char*>(&opCode), sizeof(opCode));
                if (walBin.eof()) break;  // avoid re-reading last byte

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
        }
        //flushBuffer(root);
        walBin.close();
        isReplaying = false;
        //flushBuffer(root);

    }


    ~BEpsilonTree()
    {
        saveTreeToFile(root, "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\tree_data.bin");

        if (opCounter > 0) {
            checkpoint();
        }

        // === Backup and clear binary WAL ===
        const std::string binWal = "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\nvm_tree_bin.wal";
        const std::string bakName = "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\wal_bin_backup" + timestampename() + ".bak";

        std::ifstream src(binWal, std::ios::binary);
        std::ofstream dst(bakName, std::ios::binary);
        if (src && dst) {
            dst << src.rdbuf();  // Backup binary WAL
        }
        src.close();
        dst.close();

        // Truncate original WAL file
        std::ofstream clear(binWal, std::ios::trunc | std::ios::binary);
        clear.close();
    }

private:

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

        // Insert or update the key
        lruList.push_front(key);
        readCache[key] = value;
    }


    std::string timestampename() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utcTime;
        gmtime_s(&utcTime, &time);
        std::stringstream timestamp;
        timestamp << std::put_time(&utcTime, "%Y-%m-%d_%H-%M-%S");
        return timestamp.str();
    }

    void checkpoint() {
        // Save tree to disk
        saveTreeToFile(root, "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\tree_data.bin");

        std::string backupFilename = "C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\wal_backup" + timestampename() + ".bak";

        std::ifstream src(logFilename, std::ios::binary);
        std::ofstream dst(backupFilename, std::ios::binary);
        dst << src.rdbuf();

        // Clear the WAL
        std::ofstream clearLog(logFilename, std::ios::trunc);
        clearLog.close();

        std::cout << "[CHECKPOINT] Tree saved, WAL cleared, backup created: " << backupFilename << "\n";
    }


    void maybeCheckpoint() {
        if (isReplaying) return; // Avoid checkpointing during replay

        if (++opCounter >= checkpointFrequency) {
            checkpoint();
            opCounter = 0;
        }
    }


    void logOperationBinary(Operations op, const KeyType& key, const ValueType& value = ValueType{}) {
        if (isReplaying) return;
        std::ofstream log("C:\\Users\\zarroa\\Desktop\\B-Epsilon_Tree\\nvm_tree_bin.wal", std::ios::binary | std::ios::app);
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


    template <typename KeyType, typename ValueType>
    void deleteTree(std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        if (!node) return;

        // Recursively delete all children
        for (std::shared_ptr<Node<KeyType, ValueType>> child : node->children)
        {
            deleteTree(child);
        }

        //delete node;
        node.reset();
    }


    // Function to save the tree to a file
    void saveTreeToFile(const std::shared_ptr<Node<KeyType, ValueType>>& root, const std::string& filename)
    {
        std::ofstream outFile(filename, std::ios::binary);
        if (!outFile)
        {
            std::cerr << " Error: Could not create file '" << filename << "' for writing!" << std::endl;
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
            outFile.write(reinterpret_cast<char*>(node->keys.data()), keyCount * sizeof(KeyType));

            if (isLeaf)
            {
                // Only leaf nodes store values
                size_t valueCount = node->values.size();
                outFile.write(reinterpret_cast<char*>(&valueCount), sizeof(size_t));
                outFile.write(reinterpret_cast<char*>(node->values.data()), valueCount * sizeof(ValueType));
            }

            if (!isLeaf)
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


    // Function to load the tree from a file
    std::shared_ptr<Node<KeyType, ValueType>> loadTreeFromFile(const std::string& filename)
    {
        std::ifstream inFile(filename, std::ios::binary);
        if (!inFile)
        {
            std::cerr << " Error opening file for reading!" << std::endl;
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

            if (inFile.eof() || inFile.fail())
            {
                std::cerr << " Error reading node metadata! File may be corrupted." << std::endl;
                return nullptr;
            }

            node->isLeaf = isLeaf;
            node->keys.resize(keyCount);
            inFile.read(reinterpret_cast<char*>(node->keys.data()), keyCount * sizeof(KeyType));

            if (isLeaf)
            {
                size_t valueCount;
                inFile.read(reinterpret_cast<char*>(&valueCount), sizeof(size_t));

                if (valueCount != keyCount)
                {
                    std::cerr << " ERROR: Value count mismatch! Keys: " << keyCount << ", Values: " << valueCount << std::endl;
                    return nullptr;
                }

                node->values.resize(valueCount);
                inFile.read(reinterpret_cast<char*>(node->values.data()), valueCount * sizeof(ValueType));
            }

            if (!isLeaf)
            {
                size_t childCount;
                inFile.read(reinterpret_cast<char*>(&childCount), sizeof(size_t));

                for (size_t i = 0; i < childCount; ++i) {
                    auto child = std::make_shared<Node<KeyType, ValueType>>(true);
                    node->children.push_back(child);
                    q.push(child);
                }
            }
        }
        inFile.close();
        return root;
    }

    KeyType getNodeKey(const std::shared_ptr<Node<KeyType, ValueType>>& node) {
        return (!node->keys.empty()) ? node->keys[0] : KeyType{};
    }


public:
    // Remove a key from the tree
    template <typename KeyType>
    ErrorCode remove(KeyType key) {
        if (!root)
        {
            return ErrorCode::KeyDoesNotExist; // Tree is empty
        }
        if (isReplaying) return ErrorCode::Success;  // Prevent mutation during WAL replay
        logOperationBinary(Operations::Delete, key);

        std::shared_ptr<Node<KeyType, ValueType>> current = root;

        if (!current->isLeaf)
        {
            ErrorCode result = insertBuffered(current, Operations::Delete, key, ValueType{});
            if (result != ErrorCode::Success) return result;

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        // If we are in a leaf node, remove the key directly
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

        // Handle underflow if necessary
        if (current->keys.size() < (m_nDegree / 2))
        {
            std::shared_ptr<Node<KeyType, ValueType>> parent = findParent(root, current);
            ErrorCode result = handleUnderflow(parent, current);
            if (result != ErrorCode::Success) return result;
        }

        maybeCheckpoint();
        return ErrorCode::Success;
    }


    // Handle underflow in a node
    template <typename KeyType, typename ValueType>
    ErrorCode handleUnderflow(std::shared_ptr<Node<KeyType, ValueType>> parent, std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        if (!parent) return ErrorCode::Success; // Root doesn't underflow

        // Find the sibling of the underflowed node
        size_t index = std::find(parent->children.begin(), parent->children.end(), node) - parent->children.begin();
        std::shared_ptr<Node<KeyType, ValueType>> leftSibling = (index > 0) ? parent->children[index - 1] : nullptr;
        std::shared_ptr<Node<KeyType, ValueType>> rightSibling = (index + 1 < parent->children.size()) ? parent->children[index + 1] : nullptr;

        // Try to borrow from left sibling
        if (leftSibling && leftSibling->keys.size() > (m_nDegree / 2))
        {
            // Borrow the largest key from the left sibling
            KeyType borrowedKey = leftSibling->keys.back();
            ValueType borrowedValue = leftSibling->values.back();

            leftSibling->keys.pop_back();
            leftSibling->values.pop_back();

            node->keys.insert(node->keys.begin(), borrowedKey);
            node->values.insert(node->values.begin(), borrowedValue);

            // Update the parent's separator key
            parent->keys[index - 1] = borrowedKey;

            return ErrorCode::Success;
        }

        // Try to borrow from right sibling
        if (rightSibling && rightSibling->keys.size() > (m_nDegree / 2))
        {
            // Borrow the smallest key from the right sibling
            KeyType borrowedKey = rightSibling->keys.front();
            ValueType borrowedValue = rightSibling->values.front();

            rightSibling->keys.erase(rightSibling->keys.begin());
            rightSibling->values.erase(rightSibling->values.begin());

            node->keys.push_back(borrowedKey);
            node->values.push_back(borrowedValue);

            // Update the parent's separator key
            parent->keys[index] = rightSibling->keys.front();

            return ErrorCode::Success;
        }

        // If borrowing is not possible, merge with a sibling
        if (leftSibling)
        {
            mergeNodes(parent, leftSibling, node, index - 1);
        }
        else if (rightSibling)
        {
            mergeNodes(parent, node, rightSibling, index);
        }

        return ErrorCode::Success;
    }


    // Merge two nodes
    template <typename KeyType, typename ValueType>
    void mergeNodes(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> left,
        std::shared_ptr<Node<KeyType, ValueType>> right,
        size_t separatorIndex)
    {
        if (!left || !right || !parent) return;

        if (separatorIndex >= parent->keys.size()) {
            std::cerr << "[ERROR] Invalid separatorIndex in merge.\n";
            return;
        }

        if (right->keys.empty() && left->keys.empty()) {
            std::cerr << "[WARN] Both nodes empty. Nothing to merge.\n";
            return;
        }

        // Safe to access separator key
        if (!left->isLeaf)
            left->keys.push_back(parent->keys[separatorIndex]);

        // Merge keys/values if available
        left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
        left->values.insert(left->values.end(), right->values.begin(), right->values.end());

        // Merge children if internal node
        if (!left->isLeaf && !right->isLeaf) {
            left->children.insert(left->children.end(), right->children.begin(), right->children.end());
        }

        // Clean up parent
        if (separatorIndex < parent->keys.size())
            parent->keys.erase(parent->keys.begin() + separatorIndex);

        if ((separatorIndex + 1) < parent->children.size())
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





    // Update the value of a specific key in the tree
    template <typename KeyType, typename ValueType>
    ErrorCode update(KeyType key, ValueType newValue)
    {
        if (isReplaying) return ErrorCode::Success;  // Prevent mutation during WAL replay
        logOperationBinary(Operations::Update, key, newValue);
        std::shared_ptr<Node<KeyType, ValueType>> current = root;

        if (!current->isLeaf)
        {
            ErrorCode result = insertBuffered(current, Operations::Update, key, newValue);
            if (result != ErrorCode::Success) return result;

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        // Update directly in the leaf if found
        auto it = std::find(current->keys.begin(), current->keys.end(), key);
        if (it != current->keys.end())
        {
            int index = std::distance(current->keys.begin(), it);
            current->values[index] = newValue; // Update the value
        }
        else
        {
            return ErrorCode::KeyDoesNotExist; // Key not found
        }

        maybeCheckpoint();
        return ErrorCode::Success;
    }


    // Search for a key in the tree
    template <typename KeyType, typename ValueType>
    ErrorCode search(KeyType key, ValueType& value)
    {
        // DRAM Read Cache Lookup
        auto cacheIt = readCache.find(key);
        if (cacheIt != readCache.end()) {
            // Update LRU position
            lruList.remove(key);
            lruList.push_front(key);

            value = cacheIt->second;
            return ErrorCode::Success;
        }

        if (!root) return ErrorCode::KeyDoesNotExist;

        std::shared_ptr<Node<KeyType, ValueType>> current = root;

        // Track if there's a message in the global buffer for this key
        std::optional<std::tuple<Operations, KeyType, ValueType>> bufferedMessage;
        auto it = sharedBuffer.find(key);
        if (it != sharedBuffer.end())
            bufferedMessage = it->second;

        // Traverse down to the leaf
        while (!current->isLeaf)
        {
            size_t i = std::upper_bound(current->keys.begin(), current->keys.end(), key) - current->keys.begin();
            if (i >= current->children.size()) return ErrorCode::KeyDoesNotExist;
            current = current->children[i];
        }

        // Search the leaf node
        auto keyIt = std::find(current->keys.begin(), current->keys.end(), key);
        if (keyIt != current->keys.end())
        {
            size_t index = std::distance(current->keys.begin(), keyIt);
            value = current->values[index];
        }
        else
        {
            if (bufferedMessage.has_value())
            {
                auto [op, _, val] = bufferedMessage.value();
                if (op == Operations::Insert)
                {
                    value = val;
                    return ErrorCode::Success;
                }
                else if (op == Operations::Delete)
                {
                    return ErrorCode::KeyDoesNotExist;
                }
            }
            else
            {
                return ErrorCode::KeyDoesNotExist;
            }
        }

        // Apply Update if exists
        if (bufferedMessage.has_value())
        {
            auto [op, _, val] = bufferedMessage.value();
            if (op == Operations::Update)
            {
                value = val;
            }
            else if (op == Operations::Delete)
            {
                return ErrorCode::KeyDoesNotExist;
            }
        }
        updateReadCache(key, value);


        return ErrorCode::Success;
    }



    // Range query for keys in the range [low, high]
    template <typename KeyType>
    std::vector<std::pair<KeyType, ValueType>> rangeQuery(KeyType low, KeyType high)
    {
        std::vector<std::pair<KeyType, ValueType>> result;
        std::shared_ptr<Node<KeyType, ValueType>> current = root;

        // Traverse to the first relevant leaf node
        while (!current->isLeaf)
        {
            size_t i = std::upper_bound(current->keys.begin(), current->keys.end(), low) - current->keys.begin();
            if (i >= current->children.size()) break;
            current = current->children[i];
        }

        // Traverse leaf nodes & collect values in range
        while (current)
        {
            for (size_t i = 0; i < current->keys.size(); ++i)
            {
                KeyType k = current->keys[i];
                if (k > high) goto Done;
                if (k >= low)
                {
                    result.emplace_back(k, current->values[i]);
                }
            }

            // Move to next leaf
            std::shared_ptr<Node<KeyType, ValueType>> parent = findParent(root, current);
            while (parent)
            {
                size_t index = std::find(parent->children.begin(), parent->children.end(), current) - parent->children.begin();
                if (index + 1 < parent->children.size())
                {
                    current = parent->children[index + 1];
                    while (!current->isLeaf) current = current->children[0];
                    break;
                }
                else
                {
                    current = parent;
                    parent = findParent(root, parent);
                }
            }

            if (!parent) break;
        }

    Done:
        // Overlay centralized buffer
        for (const auto& [key, msg] : sharedBuffer)
        {
            if (key < low || key > high) continue;

            auto [op, _, val] = msg;

            if (op == Operations::Insert || op == Operations::Update)
            {
                // Replace if exists
                auto it = std::find_if(result.begin(), result.end(),
                    [key](const auto& p) { return p.first == key; });

                if (it != result.end())
                    it->second = val;  // Update existing
                else
                    result.emplace_back(key, val);  // New entry
            }
            else if (op == Operations::Delete)
            {
                // Remove if exists
                auto it = std::remove_if(result.begin(), result.end(),
                    [key](const auto& p) { return p.first == key; });
                result.erase(it, result.end());
            }
        }

        // Sort and deduplicate
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end(),
            [](const auto& a, const auto& b) {
                return a.first == b.first;
            }),
            result.end());

        return result;
    }



    // Insert a key-value pair into the tree
    template <typename KeyType, typename ValueType>
    ErrorCode insert(KeyType key, ValueType value)
    {
        if (isReplaying) return ErrorCode::Success;  // Prevent mutation during WAL replay
        logOperationBinary(Operations::Insert, key, value);

        // Case: Tree is empty
        if (!root)
        {
            root = std::make_shared<Node<KeyType, ValueType>>(true);
            root->keys.push_back(key); // Insert the key directly
            root->values.push_back(value); // Insert the value directly
            maybeCheckpoint();
            return ErrorCode::Success;
        }

        std::shared_ptr<Node<KeyType, ValueType>> current = root;

        // Traverse the tree to find the appropriate node
        if (!current->isLeaf)
        {
            // Case: Add to the buffer of the root or current node
            ErrorCode result = insertBuffered(current, Operations::Insert, key, value);
            if (result != ErrorCode::Success)
            {
                return result;
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        else
        {
            // Case: Leaf Node
            // Find the correct position to insert the key while maintaining sorted order
            auto it_keys = lower_bound(current->keys.begin(), current->keys.end(), key);
            //int index = it - current->keys.begin();
            //auto it_values = lower_bound(current->values.begin(), current->values.end(), key);
            auto it_values = lower_bound(current->values.begin(), current->values.end(), value);

            // Insert the key and value at the determined position
            current->keys.insert(it_keys, key);
            current->values.insert(it_values, value);
            //sort(current->values.begin(), current->values.end());

            // If the leaf is overfull, split it
            if (current->keys.size() >= m_nDegree)
            {
                std::shared_ptr<Node<KeyType, ValueType>> parent = findParent(root, current);
                ErrorCode result = splitLeaf(parent, current);
                if (result != ErrorCode::Success)
                {
                    return result;
                }
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }
    }


    // Insert an operation to the buffer of an Internal node
    template <typename KeyType, typename ValueType>
    ErrorCode insertBuffered(std::shared_ptr<Node<KeyType, ValueType>> node, Operations op, KeyType key, ValueType value)
    {
        if (sharedBuffer.size() >= maxBufferSize) {
            ErrorCode result = flushLFUNode();
            if (result != ErrorCode::Success) return result;
        }

        auto it = sharedBuffer.find(key);

        if (it != sharedBuffer.end()) {
            auto [prevOp, _, prevVal] = it->second;

            // === Coalescing Rules ===

            // INSERT - UPDATE => Merge into INSERT
            if (prevOp == Operations::Insert && op == Operations::Update) {
                sharedBuffer[key] = { Operations::Insert, key, value };
            }
            // INSERT - DELETE => Cancel the insert (net zero)
            else if (prevOp == Operations::Insert && op == Operations::Delete) {
                sharedBuffer.erase(it);
                messageToNodeMap.erase(key);
                auto& keyList = nodeMessageMap[node];
                keyList.erase(std::remove(keyList.begin(), keyList.end(), key), keyList.end());
                return ErrorCode::Success;
            }
            // UPDATE - DELETE => Keep only DELETE
            else if (prevOp == Operations::Update && op == Operations::Delete) {
                sharedBuffer[key] = { Operations::Delete, key, ValueType{} };
            }
            // UPDATE - UPDATE => Keep most recent update
            else if (prevOp == Operations::Update && op == Operations::Update) {
                sharedBuffer[key] = { Operations::Update, key, value };
            }
            // DELETE - INSERT => Transform into UPSERT (Insert)
            else if (prevOp == Operations::Delete && op == Operations::Insert) {
                sharedBuffer[key] = { Operations::Insert, key, value };
            }
            // Otherwise: overwrite
            else {
                sharedBuffer[key] = { op, key, value };
            }
        }
        sharedBuffer[key] = { op, key, value };
        nodeMessageMap[node].push_back(key);
        messageToNodeMap[key] = node;

        KeyType nodeKey = getNodeKey(node);
        nodeFrequency[nodeKey]++;

        return ErrorCode::Success;
    }

    ErrorCode flushLFUNode()
    {
        if (nodeMessageMap.empty()) return ErrorCode::Success;

        // Step 1: Get the least frequently used nodeKey
        auto minIt = std::min_element(
            nodeFrequency.begin(), nodeFrequency.end(),
            [](const auto& a, const auto& b) {
                return a.second < b.second;
            });

        if (minIt == nodeFrequency.end()) return ErrorCode::Error;

        KeyType lfuKey = minIt->first;

        // Step 2: Find the node in nodeMessageMap with that key
        auto nodeIt = std::find_if(nodeMessageMap.begin(), nodeMessageMap.end(),
            [&](const auto& pair) {
                return !pair.first->keys.empty() && pair.first->keys[0] == lfuKey;
            });

        if (nodeIt == nodeMessageMap.end()) return ErrorCode::Error;

        auto nodeToFlush = nodeIt->first;

        // Step 3: Flush and clean up
        ErrorCode result = flushBuffer(nodeToFlush);
        if (result == ErrorCode::Success) {
            nodeFrequency.erase(lfuKey);
        }

        return result;
    }



    template <typename KeyType, typename ValueType>
    ErrorCode flushBuffer(std::shared_ptr<Node<KeyType, ValueType>> node)
    {
        // 1) If it's a leaf, there's no "buffer" to flush downward
        if (node->isLeaf)
            return ErrorCode::Success;

        // 2) Check if this node actually has any messages in nodeMessageMap
        auto it = nodeMessageMap.find(node);
        if (it == nodeMessageMap.end()) {
            // No messages for this node
            return ErrorCode::Success;
        }

        // 3) Copy the list of keys we need to flush, then remove the mapping entry
        std::vector<KeyType> keysCopy = it->second;
        nodeMessageMap.erase(it);

        // 4) For each key in keysCopy, find the message in sharedBuffer and flush it
        for (KeyType key : keysCopy)
        {
            auto msgIt = sharedBuffer.find(key);
            if (msgIt == sharedBuffer.end()) {
                // Possibly it got coalesced away or re-labeled; skip
                continue;
            }

            // Extract the operation info
            auto [opType, msgKey, msgValue] = msgIt->second;

            // Determine which child node should handle this key
            size_t i = std::upper_bound(node->keys.begin(), node->keys.end(), msgKey)
                - node->keys.begin();
            if (i >= node->children.size()) {
                // Key is out of range? Possibly an edge case
                continue;
            }

            std::shared_ptr<Node<KeyType, ValueType>> child = node->children[i];

            // 5) If the child is also an internal node, re-insert this message for the child
            //    If the child is a leaf, physically apply the operation
            if (!child->isLeaf) {
                // Re-label the message so it belongs to the child
                // This call adds (opType, msgKey, msgValue) to child's portion of the global buffer
                ErrorCode result = insertBuffered(child, opType, msgKey, msgValue);
                if (result != ErrorCode::Success) {
                    // If an error, you might want to handle or log it
                    return result;
                }
            }
            else {
                // Child is a leaf => apply the operation directly
                switch (opType)
                {
                case Operations::Insert:
                {
                    // Insert msgKey/msgValue in ascending order
                    auto it_keys = std::lower_bound(child->keys.begin(), child->keys.end(), msgKey);
                    auto it_values = std::lower_bound(child->values.begin(), child->values.end(), msgValue);
                    child->keys.insert(it_keys, msgKey);
                    child->values.insert(it_values, msgValue);

                    // If the leaf is overfull, do a split
                    if (child->keys.size() >= m_nDegree) {
                        auto parentOfChild = findParent(root, child);
                        splitLeaf(parentOfChild, child);
                    }
                    break;
                }
                case Operations::Delete:
                {
                    // Remove msgKey if it exists in the child
                    auto delIt = std::find(child->keys.begin(), child->keys.end(), msgKey);
                    if (delIt != child->keys.end()) {
                        size_t delIdx = std::distance(child->keys.begin(), delIt);
                        child->keys.erase(delIt);
                        child->values.erase(child->values.begin() + delIdx);

                        // If the leaf underflows, handle it
                        if (child->keys.size() < (m_nDegree / 2)) {
                            auto parentOfChild = findParent(root, child);
                            handleUnderflow(parentOfChild, child);
                        }
                    }
                    break;
                }
                case Operations::Update:
                {
                    // Find msgKey and update its value if present
                    auto updIt = std::find(child->keys.begin(), child->keys.end(), msgKey);
                    if (updIt != child->keys.end()) {
                        size_t updIdx = std::distance(child->keys.begin(), updIt);
                        child->values[updIdx] = msgValue;
                    }
                    break;
                }
                default:
                    // e.g. Upsert or unknown op
                    // Could handle similarly or log a warning
                    break;
                }
            }

            // 6) Message cleanup logic based on NVM-awareness
            if (child->isLeaf) {
                // Message applied to a node outside NVM -> remove from buffer
                sharedBuffer.erase(msgIt);
                messageToNodeMap.erase(key);
            }
            else {
                // Message is still in NVM tree, only relabeled -> do not erase
                messageToNodeMap[key] = child;
                nodeMessageMap[child].push_back(key);
            }

        }

        return ErrorCode::Success;
    }


    // propagate an operation to the buffer of a child node
    template <typename KeyType, typename ValueType>
    ErrorCode propagateToBuffer(std::shared_ptr<Node<KeyType, ValueType>> child, Operations opType, KeyType key, ValueType value)
    {
        return insertBuffered(child, opType, key, value);
    }


    // split a leaf node
    template <typename KeyType, typename ValueType>
    ErrorCode splitLeaf(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> leaf)
    {
        // Create a new sibling node
        std::shared_ptr<Node<KeyType, ValueType>> sibling = std::make_shared<Node<KeyType, ValueType>>(true);

        // Determine the midpoint to split
        int mid = static_cast<int>(leaf->keys.size()) / 2;

        // Step 3: Move half of the keys/values to the sibling
        sibling->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
        sibling->values.assign(leaf->values.begin() + mid, leaf->values.end());

        // Resize the original leaf to keep only the first half
        leaf->keys.resize(mid);
        leaf->values.resize(mid);

        // Determine the pivot key (smallest key in the sibling)
        KeyType pivotKey = sibling->keys[0];

        // Reassign buffered messages to the sibling if key >= pivotKey
        auto it = nodeMessageMap.find(leaf);
        if (it != nodeMessageMap.end()) {
            std::vector<KeyType> keysToMove;
            for (const KeyType& k : it->second) {
                if (k >= pivotKey) {
                    keysToMove.push_back(k);
                }
            }

            // Move those keys to sibling and update the mappings
            for (const KeyType& k : keysToMove) {
                // Remove from leaf's list
                auto& list = nodeMessageMap[leaf];
                list.erase(std::remove(list.begin(), list.end(), k), list.end());

                // Add to sibling's list
                nodeMessageMap[sibling].push_back(k);

                // Update key's responsible node
                messageToNodeMap[k] = sibling;
            }
        }

        // If no parent, create a new root
        if (!parent)
        {
            std::shared_ptr<Node<KeyType, ValueType>> newRoot = std::make_shared<Node<KeyType, ValueType>>(false);
            newRoot->keys.push_back(pivotKey);
            newRoot->children.push_back(leaf);
            newRoot->children.push_back(sibling);
            root = newRoot;
        }
        else
        {
            // Insert pivotKey into parent and place sibling to the right of leaf
            auto it = std::lower_bound(parent->keys.begin(), parent->keys.end(), pivotKey);
            parent->keys.insert(it, pivotKey);

            auto childIt = std::find(parent->children.begin(), parent->children.end(), leaf);
            parent->children.insert(childIt + 1, sibling);

            // Check if the parent also needs to split
            if (parent->keys.size() >= m_nDegree)
            {
                std::shared_ptr<Node<KeyType, ValueType>> grandparent = findParent(root, parent);
                return splitInternal(grandparent, parent);
            }
        }

        return ErrorCode::Success;
    }



    // split an internal node
    template <typename KeyType, typename ValueType>
    ErrorCode splitInternal(std::shared_ptr<Node<KeyType, ValueType>> parent,
        std::shared_ptr<Node<KeyType, ValueType>> internal)
    {
        // Create a new sibling node
        std::shared_ptr<Node<KeyType, ValueType>> sibling = std::make_shared<Node<KeyType, ValueType>>(false);

        // Determine the split point
        int mid = static_cast<int>(internal->keys.size()) / 2;

        // The middle key becomes the pivot key
        KeyType pivotKey = internal->keys[mid];

        // Move keys and children from 'internal' to 'sibling'
        sibling->keys.assign(internal->keys.begin() + mid + 1, internal->keys.end());
        sibling->children.assign(internal->children.begin() + mid + 1, internal->children.end());

        // Resize the old internal node
        internal->keys.resize(mid);
        internal->children.resize(mid + 1);

        // If there is no parent, then the 'internal' node was the root
        if (!parent)
        {
            // Create a new root
            std::shared_ptr<Node<KeyType, ValueType>> newRoot = std::make_shared<Node<KeyType, ValueType>>(false);

            // Promote the pivot key into the new root
            newRoot->keys.push_back(pivotKey);

            // The old internal node becomes the left child
            newRoot->children.push_back(internal);

            // The sibling becomes the right child
            newRoot->children.push_back(sibling);

            // Update root
            root = newRoot;
        }
        else
        {
            // Insert 'pivotKey' into the parent
            auto it = std::lower_bound(parent->keys.begin(), parent->keys.end(), pivotKey);
            parent->keys.insert(it, pivotKey);

            // Link the sibling to the parent, just after 'internal'
            auto childIt = std::find(parent->children.begin(), parent->children.end(), internal);
            if (childIt == parent->children.end())
            {
                // Parent-child relationship broken (unlikely but check anyway)
                return ErrorCode::Error;
            }
            parent->children.insert(childIt + 1, sibling);

            // Check if the parent also needs to split
            if (parent->keys.size() >= m_nDegree)
            {
                std::shared_ptr<Node<KeyType, ValueType>> grandparent = findParent(root, parent);
                return splitInternal(grandparent, parent);
            }
        }

        //
        // *** Message reassignment step ***
        //
        // All keys in nodeMessageMap[internal] that are >= pivotKey
        // now belong to the sibling, because those keys logically
        // fall in the sibling’s range.
        //
        {
            auto& oldList = nodeMessageMap[internal];  // keys currently mapped to 'internal'
            std::vector<KeyType> keysMoving;

            // Identify which keys belong to sibling
            for (const KeyType& k : oldList)
            {
                if (k >= pivotKey)
                {
                    keysMoving.push_back(k);
                }
            }

            // Move them to sibling
            for (const KeyType& k : keysMoving)
            {
                // Remove from old node's vector
                auto& listRef = nodeMessageMap[internal];
                listRef.erase(std::remove(listRef.begin(), listRef.end(), k), listRef.end());

                // Add to sibling's vector
                nodeMessageMap[sibling].push_back(k);

                // Update messageToNodeMap so that k points to sibling
                messageToNodeMap[k] = sibling;
            }
        }

        return ErrorCode::Success;
    }



    // Handle root split
    template <typename KeyType, typename ValueType>
    ErrorCode handleRootSplit(std::shared_ptr<Node<KeyType, ValueType>> oldRoot, std::shared_ptr<Node<KeyType, ValueType>> sibling, KeyType pivotKey)
    {
        std::shared_ptr<Node<KeyType, ValueType>> newRoot = std::make_shared<Node<KeyType, ValueType>>(false);
        newRoot->keys.push_back(pivotKey); // Promote the pivot key
        newRoot->children.push_back(oldRoot); // Add old root as left child
        newRoot->children.push_back(sibling); // Add sibling as right child
        root = newRoot; // Update the root
        return ErrorCode::Success;
    }


    // display the tree
    template <typename KeyType, typename ValueType>
    void display(std::shared_ptr<Node<KeyType, ValueType>> node, int level)
    {
        if (!node) return;

        // Print the keys of the current node
        cout << string(level * 2, ' ') << "[";
        for (size_t i = 0; i < node->keys.size(); i++)
        {
            if (!node->isLeaf)
            {
                cout << node->keys[i] << " ";
            }
            else
            {
                cout << node->keys[i] << ":" << node->values[i] << " ";
            }
        }
        cout << "]";
        cout << endl;

        // Recursively display child nodes
        for (std::shared_ptr<Node<KeyType, ValueType>> child : node->children)
        {
            display(child, level + 1);
        }
    }

    // template <typename KeyType, typename ValueType>
    void printSharedBuffer() const {
        std::cout << "\n--- [DEBUG] Shared Buffer Contents ---\n";
        if (sharedBuffer.empty()) {
            std::cout << "(empty)\n";
            return;
        }

        for (const auto& [key, message] : sharedBuffer)
        {
            Operations opType = std::get<0>(message);
            int k = std::get<1>(message);
            int v = std::get<2>(message);

            switch (opType)
            {
            case Operations::Insert:
                std::cout << "INSERT(" << k << " : " << v << ")\n";
                break;
            case Operations::Update:
                std::cout << "UPDATE(" << k << " : " << v << ")\n";
                break;
            case Operations::Delete:
                std::cout << "DELETE(" << k << ")\n";
                break;
            default:
                std::cout << "UNKNOWN(" << k << ")\n";
            }
        }
    }

    // template <typename KeyType, typename ValueType>
    void printNodeMessageMap() const {
        std::cout << "\n--- [DEBUG] nodeMessageMap Contents ---\n";
        if (nodeMessageMap.empty()) {
            std::cout << "(empty)\n";
            return;
        }

        for (const auto& [node, keys] : nodeMessageMap) {
            // Print internal node keys
            std::cout << "Node ";
            for (const auto& k : node->keys) {
                std::cout << k << " ";
            }
            std::cout << "has messages for Nodes: ";

            // Print buffered keys for this node
            for (const KeyType& key : keys) {
                std::cout << key << " ";
            }
            std::cout << "\n";
        }
    }


    // template <typename KeyType, typename ValueType>
    void printMessageToNodeMap() const {
        std::cout << "\n--- [DEBUG] messageToNodeMap Contents ---\n";
        if (messageToNodeMap.empty()) {
            std::cout << "(empty)\n";
            return;
        }

        for (const auto& [key, node] : messageToNodeMap) {
            std::cout << "Key " << key << " belongs to Node" << node.get() << "\n";
        }
    }


    void printNodeFrequency() const {
        std::cout << "\n--- [DEBUG] Node Frequency ---\n";
        if (nodeFrequency.empty()) {
            std::cout << "(empty)\n";
            return;
        }

        for (const auto& [key, freq] : nodeFrequency) {
            std::cout << "Node[" << key << "] -> freq = " << freq << "\n";
        }
    }

    void printReadCache() const {
    std::cout << "\n--- [DEBUG] DRAM Read Cache ---\n";
    if (readCache.empty()) {
        std::cout << "(empty)\n";
        return;
    }

    for (const auto& [key, val] : readCache) {
        std::cout << "Key: " << key << ", Value: " << val << "\n";
    }
}




    // find the parent of a child node
    template <typename KeyType, typename ValueType>
    std::shared_ptr<Node<KeyType, ValueType>> findParent(std::shared_ptr<Node<KeyType, ValueType>> current, std::shared_ptr<Node<KeyType, ValueType>> child)
    {
        if (!current || current->isLeaf) return nullptr;

        for (size_t i = 0; i < current->children.size(); i++)
        {
            if (current->children[i] == child) return current;
            std::shared_ptr<Node<KeyType, ValueType>> parent = findParent(current->children[i], child);
            if (parent) return parent;
        }
        return nullptr;
    }


    // Upsert a key-value pair into the tree
    template <typename KeyType, typename ValueType>
    ErrorCode upsert(KeyType key, ValueType value)
    {
        logOperationBinary(Operations::Upsert, key, value);

        if (!root)
        {
            root = std::make_shared<Node<KeyType, ValueType>>(true); // Create a new root as a leaf
            root->keys.push_back(key); // Insert the key directly
            root->values.push_back(value); // Insert the value directly
            maybeCheckpoint();
            return ErrorCode::Success;
        }

        std::shared_ptr<Node<KeyType, ValueType>> current = root;

        // Traverse the tree to find the appropriate node
        if (!current->isLeaf)
        {
            // Case: Add to the buffer of the root or current node
            ErrorCode result = insertBuffered(current, Operations::Insert, key, value);
            if (result != ErrorCode::Success)
            {
                return result;
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }

        else
        {
            // Case: Leaf Node
            // Find the correct position to insert the key while maintaining sorted order
            auto it = std::find(current->keys.begin(), current->keys.end(), key);

            if (it != current->keys.end())
            {
                // Update the value if the key exists

                size_t index = std::distance(current->keys.begin(), it);
                current->values[index] = value;
            }
            else
            {
                // Insert the key-value pair if the key does not exist
                auto it_keys = lower_bound(current->keys.begin(), current->keys.end(), key);
                auto it_values = lower_bound(current->values.begin(), current->values.end(), key);

                current->keys.insert(it_keys, key);
                current->values.insert(it_values, value);
                sort(current->values.begin(), current->values.end());

                // Split the leaf if it becomes overfull
                if (current->keys.size() >= m_nDegree)
                {
                    std::shared_ptr<Node<KeyType, ValueType>> parent = findParent(root, current);
                    return splitLeaf(parent, current);
                }
            }

            maybeCheckpoint();
            return ErrorCode::Success;
        }
    }

};
