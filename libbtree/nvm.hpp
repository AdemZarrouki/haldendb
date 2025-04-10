#ifndef NVM_LAYOUT_HPP
#define NVM_LAYOUT_HPP

#include <libpmemobj.h>
#include <cstdint>   // for uint8_t, etc.

// 1) Forward-declare the structs
struct message;
struct SharedBufferRoot;

// 2) Define the layout macros *before* using the structs
#ifdef _WIN32
#define LAYOUT_NAME L"bepsilon_layout"
#else
#define LAYOUT_NAME "bepsilon_layout"
#endif

POBJ_LAYOUT_BEGIN(bepsilon_layout);
POBJ_LAYOUT_ROOT(bepsilon_layout, SharedBufferRoot);
POBJ_LAYOUT_TOID(bepsilon_layout, message);
POBJ_LAYOUT_END(bepsilon_layout);

// 3) Now define your constants
#define MAX_NVM_MESSAGES 4
#define MAX_KEY_SIZE 64
#define MAX_VAL_SIZE 64

// 4) Define the actual structs
struct message {
    uint8_t opCode;
    uint32_t key_size;
    uint32_t val_size;
    char key_data[MAX_KEY_SIZE];
    char val_data[MAX_VAL_SIZE];
};

struct SharedBufferRoot {
    // typed OID from the macro above
    TOID(message) messages[MAX_NVM_MESSAGES];
    int count;
};

#endif // NVM_LAYOUT_HPP
