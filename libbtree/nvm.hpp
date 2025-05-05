#pragma once
#include <libpmemobj.h>
#include <libpmemobj/pool_base.h>
#include <libpmemobj/base.h>
#include <libpmemobj/atomic_base.h>
#include <cstdint>

struct message;
struct KeyList;
struct NodeMessageEntry;
struct MessageToNodeEntry;
struct NodeFrequencyEntry;
struct PMEMRoot;
struct PersistentNode;

#ifdef _WIN32
#define LAYOUT_NAME L"bepsilon_layout"
#else
#define LAYOUT_NAME "bepsilon_layout"
#endif

POBJ_LAYOUT_BEGIN(bepsilon_layout);
POBJ_LAYOUT_ROOT(bepsilon_layout, PMEMRoot);
POBJ_LAYOUT_TOID(bepsilon_layout, PersistentNode);
POBJ_LAYOUT_TOID(bepsilon_layout, message);
POBJ_LAYOUT_TOID(bepsilon_layout, KeyList);
POBJ_LAYOUT_TOID(bepsilon_layout, NodeMessageEntry);
POBJ_LAYOUT_TOID(bepsilon_layout, MessageToNodeEntry);
POBJ_LAYOUT_TOID(bepsilon_layout, NodeFrequencyEntry);
POBJ_LAYOUT_END(bepsilon_layout);

#define MAX_NVM_MESSAGES 1000
#define MAX_KEY_SIZE 64
#define MAX_VAL_SIZE 64
#define MAX_KEYS_PER_NODE 1000
#define NODE_BUFFER_SIZE (2 * 1024 * 1024)
#define MAX_NODES 100000
#define MAX_MESSAGES 1000000



struct message {
    uint8_t opCode;
    uint32_t key_size;
    uint32_t val_size;
    char key_data[MAX_KEY_SIZE];
    char val_data[MAX_VAL_SIZE];
};

struct KeyList {
    int count;
    uint64_t keys[MAX_KEYS_PER_NODE];
};

struct NodeMessageEntry {
    uint64_t node_id;
    KeyList key_list;
};

struct MessageToNodeEntry {
    uint64_t key;
    uint64_t node_id;
};

struct NodeFrequencyEntry {
    uint64_t node_id;
    int frequency;
};

struct PMEMRoot {
    TOID(PersistentNode) persistentRoot;

    // Shared buffer
    TOID(message) messages[MAX_NVM_MESSAGES];
    int messageCount;

    // Maps for the AuxiliaryMaps
    TOID(NodeMessageEntry) nodeMessageMap[MAX_NODES];
    int nodeMessageMapCount;

    TOID(MessageToNodeEntry) messageToNodeMap[MAX_MESSAGES];
    int messageToNodeMapCount;

    TOID(NodeFrequencyEntry) nodeFrequencyMap[MAX_NODES];
    int nodeFrequencyCount;

    int nextLeafFileID;
};


struct PersistentNode {
    uint8_t isLeaf;
    size_t keyCount;
    uint64_t keys[MAX_KEYS_PER_NODE];
    uint64_t children[MAX_KEYS_PER_NODE + 1];

    // Buffer region for update messages
    char buffer[NODE_BUFFER_SIZE];
    size_t buffer_offset;
};
