#pragma once
#ifndef DISK_LEAF_NODE_HPP
#define DISK_LEAF_NODE_HPP

#include <cstdint>
#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include "nvm.hpp"
#include <filesystem>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

struct SerializedLeafNode
{
    uint8_t isLeaf = 1;
    size_t keyCount = 0;
    uint64_t keys[MAX_KEYS_PER_NODE] = {0};
    uint64_t values[MAX_KEYS_PER_NODE] = {0};
    size_t buffer_offset = 0;
    char buffer[NODE_BUFFER_SIZE] = {0};
};

inline void ensureLeafStorageDir(const std::string &path)
{
    if (!std::filesystem::exists(path))
    {
        std::filesystem::create_directories(path);
    }
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0777);
#endif
}

inline bool loadLeafFromDisk(const std::string &path, SerializedLeafNode &outLeaf)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    in.read(reinterpret_cast<char *>(&outLeaf), sizeof(SerializedLeafNode));
    in.close();
    return true;
}

bool saveLeafToDisk(const std::string &path, const SerializedLeafNode &leaf)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.write(reinterpret_cast<const char *>(&leaf), sizeof(SerializedLeafNode));
    return true;
}

#endif // DISK_LEAF_NODE_HPP
