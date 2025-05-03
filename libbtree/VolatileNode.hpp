#pragma once
#include "nvm.hpp"
#include <iostream>
#include <cstring>

#define HOT_NODE_THRESHOLD 100
#define MAX_DRAM_MESSAGES 5

struct VolatileNode
{
    bool isLeaf;
    size_t keyCount;
    uint64_t keys[MAX_KEYS_PER_NODE];
    uint64_t children[MAX_KEYS_PER_NODE + 1];
    message dramBuffer[MAX_DRAM_MESSAGES]; // e.g., 8–32 messages
    int dramMessageCount;

    VolatileNode()
        : isLeaf(false), keyCount(0)
    {
        std::memset(keys, 0, sizeof(keys));
        std::memset(children, 0, sizeof(children));
        std::memset(dramBuffer, 0, sizeof(dramBuffer));
    }
};
