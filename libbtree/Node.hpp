#pragma once
#include <vector>
#include <tuple>
#include "Operations.h"

using namespace std;

template <typename KeyType, typename ValueType>
class Node {
public:
    bool isLeaf;
    vector<KeyType> keys;
    vector<ValueType> values;
    vector<Node*> children;
    vector<tuple<Operations, KeyType, ValueType>> buffer; // Buffer for internal nodes (key-value pairs)

    Node(bool isLeaf) : isLeaf(isLeaf) {}
    ~Node() {}
};
