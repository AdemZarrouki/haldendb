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

using namespace std;

template <typename KeyType, typename ValueType>
class BEpsilonTree 
{
public:
    Node<KeyType, ValueType>* root;
    uint32_t m_nDegree;
    uint32_t bufferSize;

    BEpsilonTree(int m_nDegree, int bufferSize) 
    {
        root = new Node<KeyType, ValueType>(true);
        this->m_nDegree = m_nDegree;
        this->bufferSize = bufferSize;
    }

    ~BEpsilonTree() 
    {
        deleteTree(root);
    }

private:
    template <typename KeyType, typename ValueType>
    void deleteTree(Node<KeyType, ValueType>* node) 
    {
        if (!node) return;

        // Recursively delete all children
        for (Node<KeyType, ValueType>* child : node->children) 
        {
            deleteTree(child);
        }

        delete node;
    }

public:

	// Remove a key from the tree
    template <typename KeyType>
    ErrorCode remove(KeyType key) {
        if (!root) 
        {
            return ErrorCode::KeyDoesNotExist; // Tree is empty
        }

        Node<KeyType, ValueType>* current = root;

        if (!current->isLeaf) 
        {
            // Add a Delete operation to the buffer
            ErrorCode result = insertBuffered(current, Operations::Delete, key, ValueType{});
            if (result != ErrorCode::Success) return result;

            // Flush the buffer if it exceeds the buffer size
            if (current->buffer.size() >= bufferSize) 
            {
                result = flushBuffer(current);
                if (result != ErrorCode::Success) return result;
            }
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
            Node<KeyType, ValueType>* parent = findParent(root, current);
            ErrorCode result = handleUnderflow(parent, current);
            if (result != ErrorCode::Success) return result;
        }

        return ErrorCode::Success;
    }


    // Handle underflow in a node
    template <typename KeyType, typename ValueType>
    ErrorCode handleUnderflow(Node<KeyType, ValueType>* parent, Node<KeyType, ValueType>* node) 
    {
        if (!parent) return ErrorCode::Success; // Root doesn't underflow

        // Find the sibling of the underflowed node
        size_t index = std::find(parent->children.begin(), parent->children.end(), node) - parent->children.begin();
        Node<KeyType, ValueType>* leftSibling = (index > 0) ? parent->children[index - 1] : nullptr;
        Node<KeyType, ValueType>* rightSibling = (index + 1 < parent->children.size()) ? parent->children[index + 1] : nullptr;

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
    void mergeNodes(Node<KeyType, ValueType>* parent, Node<KeyType, ValueType>* left, Node<KeyType, ValueType>* right, size_t separatorIndex) 
    {
        // Move the separator key from the parent to the left node
        left->keys.push_back(parent->keys[separatorIndex]);
        //left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());
        //left->values.insert(left->values.end(), right->values.begin(), right->values.end());

        if (!right->keys.size() == 1) 
        {
            // Append all keys and values from the right sibling into the left sibling
            left->keys.insert(left->keys.end(), right->keys.begin(), right->keys.end());

        }
        left->values.insert(left->values.end(), right->values.begin(), right->values.end());
        // Remove the separator key from the parent
        parent->keys.erase(parent->keys.begin() + separatorIndex);
        parent->children.erase(parent->children.begin() + separatorIndex + 1);

        delete right;
    }


    // Update the value of a specific key in the tree
    template <typename KeyType, typename ValueType>
    ErrorCode update(KeyType key, ValueType newValue) 
    {
        Node<KeyType, ValueType>* current = root;

        if (!current->isLeaf) 
        {
            // Add an Update operation to the buffer
            ErrorCode result = insertBuffered(current, Operations::Update, key, newValue);
            if (result != ErrorCode::Success) return result;

            // Flush the buffer if it exceeds the buffer size
            if (current->buffer.size() >= bufferSize) 
            {
                result = flushBuffer(current);
                if (result != ErrorCode::Success) return result;
            }
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

        return ErrorCode::Success;
    }


	// Search for a key in the tree
    template <typename KeyType, typename ValueType>
    ErrorCode search(KeyType key, ValueType& value) 
    {
        if (!root) 
        {
            return ErrorCode::KeyDoesNotExist; // Tree is empty
        }
        Node<KeyType, ValueType>* current = root;
        vector<tuple<Operations, KeyType, ValueType>> collectedMessages;

        // Traverse the tree
        while (!current->isLeaf) 
        {
            // Collect relevant messages for the key
            for (const auto& message : current->buffer) 
            {
                if (std::get<1>(message) == key) 
                {
                    collectedMessages.emplace_back(message);
                }
            }

            // Determine which child to descend into
            size_t i = std::upper_bound(current->keys.begin(), current->keys.end(), key) - current->keys.begin();
            if (i >= current->children.size()) 
            {
                return ErrorCode::KeyDoesNotExist; // Key not found
            }

            current = current->children[i];
        }

        // Now we are in a leaf node
        auto it = std::find(current->keys.begin(), current->keys.end(), key);

        if (it != current->keys.end()) 
        {
            // Key found: Start with the value in the leaf node
            size_t index = std::distance(current->keys.begin(), it);
            value = current->values[index];
        }
        else 
        {
            // Key not found in the leaf; check for an Insert message
            bool keyInserted = false;

            if (!collectedMessages.empty())
            {
                for (auto messageIt = collectedMessages.rbegin(); messageIt != collectedMessages.rend(); ++messageIt) 
                {
                    Operations opType = std::get<0>(*messageIt);
                    ValueType messageValue = std::get<2>(*messageIt);

                    if (opType == Operations::Insert) 
                    {
                        value = messageValue; // Use the value from the Insert message
                        keyInserted = true;
                        break;
                    }
                    if (opType == Operations::Delete) 
                    {
                        return ErrorCode::KeyDoesNotExist; // Key was deleted
                    }
                }
            }

            if (!keyInserted) 
            {
                return ErrorCode::KeyDoesNotExist; // Key does not exist
            }
        }

        // Apply any remaining messages in reverse order
        for (auto messageIt = collectedMessages.rbegin(); messageIt != collectedMessages.rend(); ++messageIt) 
        {
            Operations opType = std::get<0>(*messageIt);
            ValueType messageValue = std::get<2>(*messageIt);

            if (opType == Operations::Delete) 
            {
                return ErrorCode::KeyDoesNotExist; // Key was deleted
            }
            if (opType == Operations::Update) 
            {
                value = messageValue; // Apply the update
            }
        }

        return ErrorCode::Success;
    }


	// Range query for keys in the range [low, high]
    template <typename KeyType>
    std::vector<std::pair<KeyType, ValueType>> rangeQuery(KeyType low, KeyType high) 
    {
        std::vector<std::pair<KeyType, ValueType>> result;
        Node<KeyType, ValueType>* current = root;

        // Traverse to the first relevant leaf node
        while (!current->isLeaf) 
        {
            // Process the buffer of the current node
            for (const auto& message : current->buffer) 
            {
                KeyType bufferedKey = std::get<1>(message);
                if (bufferedKey >= low && bufferedKey <= high) 
                {
                    Operations opType = std::get<0>(message);
                    ValueType bufferedValue = std::get<2>(message);

                    if (opType == Operations::Insert) 
                    {
                        result.emplace_back(bufferedKey, bufferedValue);
                    }
                    else if (opType == Operations::Delete) 
                    {
                        auto it = std::remove_if(result.begin(), result.end(),
                            [bufferedKey](const std::pair<KeyType, ValueType>& pair) {
                                return pair.first == bufferedKey;
                            });
                        result.erase(it, result.end());
                    }
                }
            }

            size_t i = std::upper_bound(current->keys.begin(), current->keys.end(), low) - current->keys.begin();
            if (i >= current->children.size()) 
            {
                break;
            }

            current = current->children[i];
        }

        // Collect keys from relevant leaf nodes
        while (true) 
        {
            // Collect keys in the range
            for (size_t i = 0; i < current->keys.size(); ++i) 
            {
                if (current->keys[i] >= low && current->keys[i] <= high) 
                {
                    result.emplace_back(current->keys[i], current->values[i]);
                }
                else if (current->keys[i] > high)
                {
                    return result;
                }
            }

            // Find the next leaf node using parent-child relationship
            Node<KeyType, ValueType>* parent = findParent(root, current);
            while (parent) 
            {
                size_t index = std::find(parent->children.begin(), parent->children.end(), current) - parent->children.begin();

                // Check if there's a sibling to the right
                if (index + 1 < parent->children.size()) 
                {
                    current = parent->children[index + 1];

                    // Descend to the leftmost child of the sibling
                    while (!current->isLeaf) 
                    {
                        current = current->children[0];
                    }
                    break;
                }
                else 
                {
                    // Move up to the parent's parent
                    current = parent;
                    parent = findParent(root, parent);
                }
            }

            if (!parent) 
            {
                break;
            }
        }
        // Sort and remove duplicates
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end(),
            [](const std::pair<KeyType, ValueType>& a, const std::pair<KeyType, ValueType>& b) {
                return a.first == b.first;
            }),
            result.end());

        return result;
    }


	// Insert a key-value pair into the tree
    template <typename KeyType, typename ValueType>
    ErrorCode insert(KeyType key, ValueType value) 
    {
        // Case: Tree is empty
        if (!root) 
        {
            root = new Node<KeyType, ValueType>(true); // Create a new root as a leaf
            root->keys.push_back(key); // Insert the key directly
            root->values.push_back(value); // Insert the value directly
            return ErrorCode::Success;
        }

        Node<KeyType, ValueType>* current = root;

        // Traverse the tree to find the appropriate node
        if (!current->isLeaf) 
        {
            // Case: Add to the buffer of the root or current node
            ErrorCode result = insertBuffered(current, Operations::Insert, key, value);
            if (result != ErrorCode::Success) 
            {
                return result;
            }

            // Flush the buffer if necessary
            if (current->buffer.size() >= bufferSize) 
            {
                ErrorCode result = flushBuffer(current);
                if (result != ErrorCode::Success) 
                {
                    return result;
                }
            }
            return ErrorCode::Success;
        }

        else
        {
            // Case: Leaf Node
            // Find the correct position to insert the key while maintaining sorted order
            auto it_keys = lower_bound(current->keys.begin(), current->keys.end(), key);
            //int index = it - current->keys.begin();
            auto it_values = lower_bound(current->values.begin(), current->values.end(), key);

            // Insert the key and value at the determined position
            current->keys.insert(it_keys, key);
            current->values.insert(it_values, value);
            sort(current->values.begin(), current->values.end());

            // If the leaf is overfull, split it
            if (current->keys.size() >= m_nDegree) 
            {
                Node<KeyType, ValueType>* parent = findParent(root, current);
                ErrorCode result = splitLeaf(parent, current);
                if (result != ErrorCode::Success) 
                {
                    return result;
                }
            }
            return ErrorCode::Success;
        }
    }


    // Insert an operation to the buffer of an Internal node
    template <typename KeyType, typename ValueType>
    ErrorCode insertBuffered(Node<KeyType, ValueType>* node, Operations operation, KeyType key, ValueType value) 
    {
        auto it = std::find_if(node->buffer.begin(), node->buffer.end(),
            [key](const tuple<Operations, KeyType, ValueType>& op) {
                return std::get<1>(op) == key;
            });

        if (it != node->buffer.end()) 
        {
            Operations opType = std::get<0>(*it);

            switch (operation)
            {
            case Operations::Insert:
                if (opType == Operations::Insert) 
                {
                    // Replace the last insert with the new one
                    *it = { Operations::Insert, key, value };
                }
                else if (opType == Operations::Delete) 
                {
                    // Ignore the new insert
                    return ErrorCode::Success;
                }
                else if (opType == Operations::Update) 
                {
                    // Replace Update with Insert
                    *it = { Operations::Insert, key, value };
                }
                break;
            case Operations::Delete:
                if (opType == Operations::Insert || opType == Operations::Update) 
                {
                    // Remove the Insert operation and add Delete
                    *it = { Operations::Delete, key, ValueType{} };
                }
                else if (opType == Operations::Delete) {
                    // Do nothing, as Delete already exists
                    return ErrorCode::Success;
                }
                break;
            case Operations::Update:
                if (opType == Operations::Insert || opType == Operations::Update) 
                {
                    *it = { Operations::Update, key, value }; // Replace Insert/Update with Update
                }
                else if (opType == Operations::Delete) 
                {
                    return ErrorCode::Error; // Can't Update after Delete
                }
                break;
            default:
                return ErrorCode::Error;
            }

        }
        else
        {
            node->buffer.emplace_back(operation, key, value);
        }

        return ErrorCode::Success;
    }


	// Flush the buffer of an Internal node
    template <typename KeyType, typename ValueType>
    ErrorCode flushBuffer(Node<KeyType, ValueType>* node) 
    {
        if (node->isLeaf || node->buffer.empty()) return ErrorCode::Success;
        auto bufferCopy = node->buffer;
        node->buffer.clear();

        for (auto& operation : bufferCopy) 
        {
            Operations opType = std::get<0>(operation); // Operation type
            KeyType key = std::get<1>(operation);          // Key
            ValueType value = std::get<2>(operation);        // Value

            // Find the appropriate child
            size_t i = std::upper_bound(node->keys.begin(), node->keys.end(), key) - node->keys.begin();
            if (i >= node->children.size()) return ErrorCode::Error;

            Node<KeyType, ValueType>* child = node->children[i];

            switch (opType)
            {
            case Operations::Insert:
                if (!child->isLeaf) 
                {
                    // Propagate insert to the child buffer
                    propagateToBuffer(child, Operations::Insert, key, value);
                }
                else 
                {
                    // Insert directly into the leaf
                    auto it_keys = lower_bound(child->keys.begin(), child->keys.end(), key);
                    auto it_values = lower_bound(child->values.begin(), child->values.end(), value);

                    child->keys.insert(it_keys, key);
                    child->values.insert(it_values, value);
                    sort(child->values.begin(), child->values.end());


                    // Split leaf if necessary
                    if (child->keys.size() >= m_nDegree) 
                    {
                        ErrorCode result = splitLeaf(node, child);
                        if (result != ErrorCode::Success) return result;
                        node = findParent(root, child);
                        //node = root;
                    }

                    /*if (child->keys.size() >= m_nDegree) {
                        Node<KeyType, ValueType>* parent = findParent(root, child);
                        ErrorCode result = splitLeaf(parent, child);
                        if (result != ErrorCode::Success) {
                            return result;
                        }
                        //node = findParent(root, child);
                    }*/


                }
                break;
            case Operations::Delete:
                if (!child->isLeaf) 
                {
                    // Propagate delete to the child buffer
                    propagateToBuffer(child, Operations::Delete, key, ValueType{});
                }
                else 
                {
                    // Remove directly from the leaf
                    auto it = std::find(child->keys.begin(), child->keys.end(), key);
                    if (it != child->keys.end()) 
                    {
                        size_t index = std::distance(child->keys.begin(), it);
                        child->keys.erase(it);
                        child->values.erase(child->values.begin() + index);

                        // Handle underflow if necessary
                        if (child->keys.size() < (m_nDegree / 2)) 
                        {
                            ErrorCode result = handleUnderflow(node, child);
                            if (result != ErrorCode::Success) return result;
                        }
                    }
                }
                /*if (!child->isLeaf) {
                    propagateToBuffer(child, Operations::Delete, key, 0);
                }
                else {
                    auto it = std::find(child->keys.begin(), child->keys.end(), key);
                    if (it != child->keys.end()) {
                        size_t index = std::distance(child->keys.begin(), it);
                        child->keys.erase(it);
                        child->values.erase(child->values.begin() + index);

                        // Handle underflow
                        if (child->keys.size() < (m_nDegree + 1) / 2) {
                            ErrorCode result = handleUnderflow(child, node);
                            if (result != ErrorCode::Success) return result;
                        }
                    }
                }*/
                break;

            case Operations::Update:
                if (!child->isLeaf) 
                {
                    propagateToBuffer(child, Operations::Update, key, value);
                }
                else 
                {
                    auto it = std::find(child->keys.begin(), child->keys.end(), key);
                    if (it != child->keys.end()) 
                    {
                        size_t index = std::distance(child->keys.begin(), it);
                        child->values[index] = value; // Update value
                    }
                }
                break;
            default:
                return ErrorCode::Error;
            }
        }

        //node->buffer.clear();
        return ErrorCode::Success;
    }


	// propagate an operation to the buffer of a child node
    template <typename KeyType, typename ValueType>
    ErrorCode propagateToBuffer(Node<KeyType, ValueType>* child, Operations opType, KeyType key, ValueType value) {
        child->buffer.emplace_back(opType, key, value);
        if (child->buffer.size() >= bufferSize) 
        {
            return flushBuffer(child); // Flush child buffer if full
        }
        return ErrorCode::Success;
    }


	// split a leaf node
    template <typename KeyType, typename ValueType>
    ErrorCode splitLeaf(Node<KeyType, ValueType>* parent, Node<KeyType, ValueType>* leaf) 
    {
        Node<KeyType, ValueType>* sibling = new Node<KeyType, ValueType>(true); // Create a new sibling (leaf)

        // Determine the split point
        int mid = leaf->keys.size() / 2;

        // Split keys and values between the current leaf and the sibling
        sibling->keys.assign(leaf->keys.begin() + mid, leaf->keys.end());
        sibling->values.assign(leaf->values.begin() + mid, leaf->values.end());
        leaf->keys.resize(mid);
        leaf->values.resize(mid);

        // The smallest key in the sibling becomes the pivot key
        KeyType pivotKey = sibling->keys[0];

        if (!parent) 
        {
            // Case: Leaf node is the root, so create a new root
            Node<KeyType, ValueType>* newRoot = new Node<KeyType, ValueType>(false); // New internal root
            newRoot->keys.push_back(pivotKey); // Promote pivot key to the root
            newRoot->children.push_back(leaf); // Add current leaf as the left child
            newRoot->children.push_back(sibling); // Add sibling as the right child
            root = newRoot; // Update the root pointer
        }
        else 
        {
            // Insert pivotKey into parent and add sibling
            auto it = std::lower_bound(parent->keys.begin(), parent->keys.end(), pivotKey);
            parent->keys.insert(it, pivotKey);

            auto childIt = std::find(parent->children.begin(), parent->children.end(), leaf);
            parent->children.insert(childIt + 1, sibling);

            // Check if the parent needs to split
            if (parent->keys.size() >= m_nDegree) 
            {
                Node<KeyType, ValueType>* grandparent = findParent(root, parent);
                return splitInternal(grandparent, parent);
            }
        }

        return ErrorCode::Success;
    }


	// split an internal node
    template <typename KeyType, typename ValueType>
    ErrorCode splitInternal(Node<KeyType, ValueType>* parent, Node<KeyType, ValueType>* internal) 
    {
        Node<KeyType, ValueType>* sibling = new Node<KeyType, ValueType>(false); // Create a new sibling (internal node)

        // Determine the split point
        int mid = internal->keys.size() / 2;

        // The middle key becomes the pivot key
        KeyType pivotKey = internal->keys[mid];

        // Split keys and children between the current internal node and the sibling
        //sibling->keys.assign(internal->keys.begin() + mid + 1, internal->keys.end());
        //sibling->children.assign(internal->children.begin() + mid + 1, internal->children.end());
        //internal->keys.resize(mid); // Resize after assigning sibling keys
        //internal->children.resize(mid + 1); // Resize after assigning sibling children

        sibling->keys.assign(internal->keys.begin() + mid + 1, internal->keys.end());
        sibling->children.assign(internal->children.begin() + mid + 1, internal->children.end());

        internal->keys.resize(mid); // Keep keys up to the split point
        internal->children.resize(mid + 1); // Keep children up to the split point


        if (!parent) 
        {
            // Handle root split
            return handleRootSplit(internal, sibling, pivotKey);
        }
        else 
        {
            // Promote the pivot key to the parent
            auto it = std::lower_bound(parent->keys.begin(), parent->keys.end(), pivotKey);
            parent->keys.insert(it, pivotKey);

            // Add the sibling to the parent's children
            auto childIt = std::find(parent->children.begin(), parent->children.end(), internal);
            if (childIt == parent->children.end()) 
            {
                return ErrorCode::Error; // Parent-child relationship broken
            }
            parent->children.insert(childIt + 1, sibling);

            // Check if the parent needs to split
            if (parent->keys.size() >= m_nDegree) 
            {
                Node<KeyType, ValueType>* grandparent = findParent(root, parent);
                return splitInternal(grandparent, parent);
            }
        }

        return ErrorCode::Success;
    }

    
	// Handle root split
    template <typename KeyType, typename ValueType>
    ErrorCode handleRootSplit(Node<KeyType, ValueType>* oldRoot, Node<KeyType, ValueType>* sibling, KeyType pivotKey) 
    {
        Node<KeyType, ValueType>* newRoot = new Node<KeyType, ValueType>(false); // Create a new internal root
        newRoot->keys.push_back(pivotKey); // Promote the pivot key
        newRoot->children.push_back(oldRoot); // Add old root as left child
        newRoot->children.push_back(sibling); // Add sibling as right child
        root = newRoot; // Update the root
        return ErrorCode::Success;
    }


	// display the tree
    template <typename KeyType, typename ValueType>
    void display(Node<KeyType, ValueType>* node, int level) 
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
        if (!node->isLeaf)
        {
            cout << " (buffer: ";
            // Print the operations in the buffer
            for (auto& op : node->buffer) 
            {
                Operations opType = std::get<0>(op); // Operation type
                KeyType key = std::get<1>(op);          // Key
                ValueType value = std::get<2>(op);        // Value

                // Determine the operation type and display it
                switch (opType) 
                {
                case Operations::Insert:
                    cout << "INSERT(" << key << ":" << value << ") ";
                    break;
                case Operations::Delete:
                    cout << "DELETE(" << key << ") ";
                    break;
                case Operations::Update:
                    cout << "UPDATE(" << key << ":" << value << ") ";
                    break;
                default:
                    cout << "UNKNOWN(" << key << ":" << value << ") ";
                    break;
                }
            }
            cout << ")";
        }

        cout << endl;

        // Recursively display child nodes
        for (Node<KeyType, ValueType>* child : node->children) 
        {
            display(child, level + 1);
        }
    }


	// find the parent of a child node
    template <typename KeyType, typename ValueType>
    Node<KeyType, ValueType>* findParent(Node<KeyType, ValueType>* current, Node<KeyType, ValueType>* child) 
    {
        if (!current || current->isLeaf) return nullptr;

        for (size_t i = 0; i < current->children.size(); i++) 
        {
            if (current->children[i] == child) return current;
            Node<KeyType, ValueType>* parent = findParent(current->children[i], child);
            if (parent) return parent;
        }
        return nullptr;
    }


    // check if a tree is balanced
    template <typename KeyType, typename ValueType>
    bool isBalanced(Node<KeyType, ValueType>* node, int depth, int& leafDepth) 
    {
        if (!node) return true; // Empty tree is balanced

        // Check if it's a leaf node
        if (node->isLeaf) 
        {
            if (leafDepth == -1) 
            {
                leafDepth = depth; // Set depth for the first leaf
            }
            return depth == leafDepth; // All leaves must have the same depth
        }



        // Check buffer size for internal nodes
        if (!node->isLeaf && node->buffer.size() > bufferSize) return false;

        // Recursively check all children
        for (size_t i = 0; i < node->children.size(); ++i) 
        {
            if (!isBalanced(node->children[i], depth + 1, leafDepth)) 
            {
                return false;
            }

            // Validate key ranges between parent and child
            if (i > 0 && node->children[i - 1]->keys.back() >= node->keys[i - 1]) return false;
        }

        return true;
    }


    // call isTreeBalanced
    template <typename KeyType, typename ValueType>
    bool isTreeBalanced() 
    {
        int leafDepth = -1; // Depth of the first encountered leaf
        return isBalanced(root, 0, leafDepth);
    }


	// Upsert a key-value pair into the tree
    template <typename KeyType, typename ValueType>
	ErrorCode upsert(KeyType key, ValueType value)
	{
        if (!root)
        {
            root = new Node<KeyType, ValueType>(true); // Create a new root as a leaf
            root->keys.push_back(key); // Insert the key directly
            root->values.push_back(value); // Insert the value directly
            return ErrorCode::Success;
        }

        Node<KeyType, ValueType>* current = root;

        // Traverse the tree to find the appropriate node
        if (!current->isLeaf)
        {
            // Case: Add to the buffer of the root or current node
            ErrorCode result = insertBuffered(current, Operations::Insert, key, value);
            if (result != ErrorCode::Success)
            {
                return result;
            }

            // Flush the buffer if necessary
            if (current->buffer.size() >= bufferSize)
            {
                ErrorCode result = flushBuffer(current);
                if (result != ErrorCode::Success)
                {
                    return result;
                }
            }
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
                    Node<KeyType, ValueType>* parent = findParent(root, current);
                    return splitLeaf(parent, current);
                }
            }

            return ErrorCode::Success;
        }
	}

};
