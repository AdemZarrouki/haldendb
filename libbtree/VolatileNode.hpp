#pragma once
#include "nvm.hpp"
#include <iostream>
#include <cstring>

#define HOT_NODE_THRESHOLD 50

struct VolatileNode {
    bool isLeaf;
    size_t keyCount;
    uint64_t keys[MAX_KEYS_PER_NODE];
    uint64_t children[MAX_KEYS_PER_NODE + 1];
    
    VolatileNode()
        : isLeaf(false), keyCount(0) {
        std::memset(keys, 0, sizeof(keys));
        std::memset(children, 0, sizeof(children));
    }
};
