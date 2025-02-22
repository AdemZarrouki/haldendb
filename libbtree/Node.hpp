#pragma once
#include <vector>
#include <tuple>
#include "Operations.h"

using namespace std;

template <typename KeyType, typename ValueType>
class Node 
{
public:
    bool isLeaf;
    vector<KeyType> keys;
    vector<ValueType> values;
    //vector<Node*> children;
    //vector<tuple<Operations, KeyType, ValueType>> buffer;
    std::vector<std::shared_ptr<Node<KeyType, ValueType>>> children;  // Use shared_ptr for automatic memory management
    std::vector<std::tuple<Operations, KeyType, ValueType>> buffer;

    Node(bool isLeaf) : isLeaf(isLeaf) {}
    ~Node() {}
};
