#pragma once
#include <vector>
#include <tuple>
#include <memory>
#include "Operations.h"

template <typename KeyType, typename ValueType>
class Node
{
public:
    bool isLeaf;
    std::vector<KeyType> keys;
    std::vector<ValueType> values;
    std::vector<std::shared_ptr<Node<KeyType, ValueType>>> children;

    Node(bool isLeaf) : isLeaf(isLeaf) {}
    ~Node() {}
};
