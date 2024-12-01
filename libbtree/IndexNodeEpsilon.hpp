#pragma once
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <iterator>
#include <iostream>
#include <cmath>
#include <optional>
#include <iostream>
#include <fstream>
#include <assert.h>
#include "ErrorCodes.h"
#include "Operations.h"

using namespace std;

template <typename KeyType, typename ValueType, typename ObjectUIDType, typename DataNodeEpsilonType, uint8_t TYPE_UID>
class IndexNodeEpsilon
{
public:
	// Static UID to identify the type of the node
	static const uint8_t UID = TYPE_UID;

private:
	typedef IndexNodeEpsilon<KeyType, ValueType, ObjectUIDType, DataNodeEpsilonType, UID> SelfType;

	typedef std::vector<KeyType>::const_iterator KeyTypeIterator;
	typedef std::vector<ObjectUIDType>::const_iterator CacheKeyTypeIterator;
	typedef std::vector<std::pair<KeyType, std::pair<Operations, std::optional<ValueType>>>>::const_iterator BufferIterator;


private:
	// Vector to store pivot keys and child node UIDs
	std::vector<KeyType> m_vtPivots;
	std::vector<ObjectUIDType> m_vtChildren;

	// buffer temporarily stores operations (inserts, updates, or deletions) targeting keys in the node's subtree
	std::vector<std::pair<KeyType, std::pair<Operations, std::optional<ValueType>>>> m_vtBuffer;

	// Maximum buffer size
	size_t m_nBufferSize;

public:
	// Destructor: Clears pivot, child and buffer vectors
	~IndexNodeEpsilon()
	{
		m_vtPivots.clear();
		m_vtChildren.clear();
		m_vtBuffer.clear();

		std::cout << "IndexNodeEpsilon destroyed. All resources released." << std::endl;
	}

	// Default constructor
	IndexNodeEpsilon()
	{
		std::cout << "Default constructor called." << std::endl;
	}

	// Copy constructor: Copies pivot keys, buffer  and child UIDs from another IndexNodeEpsilon
	IndexNodeEpsilon(const IndexNodeEpsilon& source)
	{
		m_vtPivots.assign(source.m_vtPivots.begin(), source.m_vtPivots.end());
		m_vtChildren.assign(source.m_vtChildren.begin(), source.m_vtChildren.end());
		m_vtBuffer.assign(source.m_vtBuffer.begin(), source.m_vtBuffer.end());
		m_nBufferSize = source.m_nBufferSize;

		std::cout << "Copy constructor called." << std::endl;
	}

	// Constructor that deserializes the node from raw data
	IndexNodeEpsilon(const char* szData) 
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value) 
		{
			uint16_t nKeyCount = 0;
			uint32_t nBufferCount = 0;

			uint32_t nOffset = sizeof(uint8_t); // Skip node type UID

			// Ensure input size is sufficient
			if (inputSize < nOffset + sizeof(uint16_t)) 
			{
				throw std::runtime_error("Insufficient data for deserialization.");
			}

			// Deserialize the key count
			memcpy(&nKeyCount, szData + nOffset, sizeof(uint16_t));
			nOffset += sizeof(uint16_t);

			// Resize pivots and children based on key count
			m_vtPivots.resize(nKeyCount);
			m_vtChildren.resize(nKeyCount + 1);

			// Deserialize pivots
			uint32_t nKeysSize = nKeyCount * sizeof(KeyType);
			if (inputSize < nOffset + nKeysSize) 
			{
				throw std::runtime_error("Insufficient data for deserialization.");
			}
			memcpy(m_vtPivots.data(), szData + nOffset, nKeysSize);
			nOffset += nKeysSize;

			// Deserialize children (UIDs of child nodes)
			uint32_t nValuesSize = (nKeyCount + 1) * sizeof(typename ObjectUIDType::NodeUID);
			if (inputSize < nOffset + nValuesSize) 
			{
				throw std::runtime_error("Insufficient data for deserialization.");
			}
			memcpy(m_vtChildren.data(), szData + nOffset, nValuesSize);
			nOffset += nValuesSize;

			// Deserialize buffer count
			memcpy(&nBufferCount, szData + nOffset, sizeof(uint32_t));
			nOffset += sizeof(uint32_t);

			// Deserialize buffer entries
			m_vtBuffer.resize(nBufferCount);
			for (uint32_t i = 0; i < nBufferCount; ++i) 
			{
				KeyType key;
				Operations op;
				std::optional<ValueType> optValue;

				// Deserialize key
				if (inputSize < nOffset + sizeof(KeyType)) 
				{
					throw std::runtime_error("Insufficient data for deserialization.");
				}
				memcpy(&key, szData + nOffset, sizeof(KeyType));
				nOffset += sizeof(KeyType);

				// Deserialize operation
				uint8_t opRaw;
				if (inputSize < nOffset + sizeof(uint8_t)) 
				{
					throw std::runtime_error("Insufficient data for deserialization.");
				}
				memcpy(&opRaw, szData + nOffset, sizeof(uint8_t));
				nOffset += sizeof(uint8_t);
				op = static_cast<Operations>(opRaw);

				// Deserialize value if possible
				if (op == Operations::Insert || op == Operations::Update) 
				{
					ValueType value;
					if (inputSize < nOffset + sizeof(ValueType)) 
					{
						throw std::runtime_error("Insufficient data for deserialization.");
					}
					memcpy(&value, szData + nOffset, sizeof(ValueType));
					optValue = value;
					nOffset += sizeof(ValueType);
				}
				// Add the entry to the buffer
				m_vtBuffer[i] = { key, {op, optValue} };
			}
			std::cout << "Constructor that deserializes the node from raw data called." << std::endl;
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement custom de/serializer.");
		}
	}

	
	// Constructor that deserializes the node from a file stream
	IndexNodeEpsilon(std::fstream& fs) 
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value) 
		{
			// Read the number of keys
			uint16_t nKeyCount = 0;
			fs.read(reinterpret_cast<char*>(&nKeyCount), sizeof(uint16_t));
			if (!fs.read(reinterpret_cast<char*>(&nKeyCount), sizeof(uint16_t))) 
			{
				throw std::runtime_error("Failed to read number of keys.");
			}

			// Resize pivots and children based on the number of keys
			m_vtPivots.resize(nKeyCount);
			m_vtChildren.resize(nKeyCount + 1);

			// Read the pivots
			if (!fs.read(reinterpret_cast<char*>(m_vtPivots.data()), nKeyCount * sizeof(KeyType))) 
			{
				throw std::runtime_error("Failed to read pivots.");
			}

			// Read the child UIDs
			if (!fs.read(reinterpret_cast<char*>(m_vtChildren.data()), (nKeyCount + 1) * sizeof(typename ObjectUIDType::NodeUID))) 
			{
				throw std::runtime_error("Failed to read child UIDs.");
			}

			// Read the buffer count
			uint32_t nBufferCount = 0;
			if (!fs.read(reinterpret_cast<char*>(&nBufferCount), sizeof(uint32_t))) 
			{
				throw std::runtime_error("Failed to read buffer count.");
			}

			// Read the buffer entries
			m_vtBuffer.resize(nBufferCount);
			for (uint32_t i = 0; i < nBufferCount; ++i)
			{
				KeyType key;
				uint8_t opRaw; // Operations as raw byte
				std::optional<ValueType> optValue;

				// Read key
				if (!fs.read(reinterpret_cast<char*>(&key), sizeof(KeyType))) 
				{
					throw std::runtime_error("Failed to read key from buffer.");
				}

				// Read operation
				if (!fs.read(reinterpret_cast<char*>(&opRaw), sizeof(uint8_t))) 
				{
					throw std::runtime_error("Failed to read operation from buffer.");
				}
				Operations op = static_cast<Operations>(opRaw);

				// Read value for specific operations
				if (op == Operations::Insert || op == Operations::Update) 
				{
					ValueType value;
					if (!fs.read(reinterpret_cast<char*>(&value), sizeof(ValueType))) 
					{
						throw std::runtime_error("Failed to read value from buffer.");
					}
					optValue = value;
				}
				// Add to buffer
				m_vtBuffer[i] = { key, {op, optValue} };
			}
			std::cout << "Constructor that deserializes the node from a file stream called." << std::endl;
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement custom de/serializer.");
		}
	}

	// Constructor that initializes the node with iterators over pivot keys, buffer and children
	IndexNodeEpsilon(KeyTypeIterator itBeginPivots, KeyTypeIterator itEndPivots, BufferIterator itBeginBuffer, BufferIterator itEndBuffer,CacheKeyTypeIterator itBeginChildren, CacheKeyTypeIterator itEndChildren)
	{
		// Resize vectors and copy data
		m_vtPivots.assign(itBeginPivots, itEndPivots);
		m_vtBuffer.assign(itBeginBuffer, itEndBuffer);
		m_vtChildren.assign(itBeginChildren, itEndChildren);

		std::cout << "Constructor that initializes the node with iterators over pivot keys, buffer and children called." << std::endl;
		
		// Validate that the number of children is one more than the number of pivots
		if (m_vtChildren.size() != m_vtPivots.size() + 1) 
		{
			throw std::invalid_argument("Number of children must be one more than the number of pivots");
		}
	}

	// Constructor that creates an internal node with a pivot key, buffer and two child UIDs
	IndexNodeEpsilon(const KeyType& pivotKey, const std::vector<std::pair<KeyType, std::pair<Operations, ValueType>>>& buffer, const ObjectUIDType& ptrLHSNode, const ObjectUIDType& ptrRHSNode)
	{
		m_vtPivots.push_back(pivotKey);
		m_vtBuffer = buffer;     //TODO: use std::move or keep it like this?
		m_vtChildren.push_back(ptrLHSNode);
		m_vtChildren.push_back(ptrRHSNode);

		std::cout << "Constructor that creates an internal node with a pivot key, buffer and two child UIDs called." << std::endl;

		// Validate that the number of children is one more than the number of pivots
		if (m_vtChildren.size() != m_vtPivots.size() + 1) 
		{
			throw std::invalid_argument("Number of children must be one more than the number of pivots");
		}
	}

public:
	// get buffer
	inline const std::vector < std::pair<KeyType, std::pair<Operations, std::optional<ValueType>>>& getBuffer() const
	{
		return m_vtBuffer;
	}

	// get buffer size
	inline size_t getBufferSize() const
	{
		return m_vtBuffer.size();
	}

	// Writes the node's data and buffer to a file stream
	inline void writeToStream(std::fstream& fs, uint8_t& uidObjectType, uint32_t& nDataSize) const
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			// Validate the file stream
			if (!fs.good())
			{
				throw std::runtime_error("File stream is not in a valid state for writing.");
			}

			uidObjectType = SelfType::UID; // Set the UID of the node type
			uint16_t nKeyCount = static_cast<uint16_t>(m_vtPivots.size());
			uint16_t nValueCount = static_cast<uint16_t>(m_vtChildren.size());
			uint32_t nBufferCount = static_cast<uint32_t>(m_vtBuffer.size());

			// Calculate the total data size
			nDataSize 
				= sizeof(uint8_t)                              // UID
				+ sizeof(uint16_t)                             // Toatal keys
				+ nKeyCount * sizeof(KeyType)                  // Size of all Keys
				+ nValueCount * sizeof(ObjectUIDType)          // Size of all Values
				+ sizeof(uint32_t)                             // number of entires in Buffer
				+ nBufferCount * (sizeof(KeyType)              // Buffer key
					+ sizeof(uint8_t)             // Buffer operation
					+ sizeof(ValueType));          // Buffer value (if applicable)
			
			// Write UID
			fs.write(reinterpret_cast<const char*>(&uidObjectType), sizeof(uint8_t));
			if (fs.fail())
			{
				throw std::runtime_error("Failed to write UID.");
			}

			// Write number of keys
			fs.write(reinterpret_cast<const char*>(&nKeyCount), sizeof(uint16_t));
			if (fs.fail())
			{
				throw std::runtime_error("Failed to write number of keys.");
			}

			// Write keys
			fs.write(reinterpret_cast<const char*>(m_vtPivots.data()), nKeyCount * sizeof(KeyType));
			if (fs.fail())
			{
				throw std::runtime_error("Failed to write keys.");
			}

			// Write children
			fs.write(reinterpret_cast<const char*>(m_vtChildren.data()), nValueCount * sizeof(ObjectUIDType));
			if (fs.fail())
			{
				throw std::runtime_error("Failed to write children.");
			}

			// Write buffer count
			fs.write(reinterpret_cast<const char*>(&nBufferCount), sizeof(uint32_t));
			if (fs.fail())
			{
				throw std::runtime_error("Failed to write buffer count.");
			}

			// Write buffer entries
			for (const auto& entry : m_vtBuffer)
			{
				// Write key
				fs.write(reinterpret_cast<const char*>(&entry.first), sizeof(KeyType));
				if (fs.fail())
				{
					throw std::runtime_error("Failed to write buffer key.");
				}

				// Write operation
				uint8_t opRaw = static_cast<uint8_t>(entry.second.first);
				fs.write(reinterpret_cast<const char*>(&opRaw), sizeof(uint8_t));
				if (fs.fail())
				{
					throw std::runtime_error("Failed to write buffer operation.");
				}

				// Write value if the operation is Insert or Update
				if ((entry.second.first == Operations::Insert || entry.second.first == Operations::Update) &&
					entry.second.second.has_value())
				{
					fs.write(reinterpret_cast<const char*>(&entry.second.second.value()), sizeof(ValueType));
					if (fs.fail())
					{
						throw std::runtime_error("Failed to write buffer value.");
					}
				}
			}
#ifdef __VALIDITY_CHECK__
			for (auto it = m_vtChildren.begin(); it != m_vtChildren.end(); it++)
			{
				assert((*it).getMediaType() >= 2);
			}
#endif //__VALIDITY_CHECK__
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement a custom serializer.");
		}
	}

	// Serializes the node's data into a char buffer
	inline void serialize(char*& szBuffer, uint8_t & uidObjectType, uint32_t & nBufferSize) const
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value) 
		{
			uidObjectType = UID;

			uint16_t nKeyCount = m_vtPivots.size();
			uint16_t nValueCount = m_vtChildren.size();
			uint32_t nBufferCount = m_vtBuffer.size();

			// Calculate the total size of the serialized data
			nBufferSize
				= sizeof(uint8_t)                              // UID
				+ sizeof(uint16_t)                             // Toatal keys
				+ nKeyCount * sizeof(KeyType)                  // Size of all Keys
				+ nValueCount * sizeof(ObjectUIDType)          // Size of all Values
				+ sizeof(uint32_t)                             // number of entires in Buffer
				+ nBufferCount * (sizeof(KeyType)              // Buffer key
					+ sizeof(uint8_t)             // Buffer operation
					+ sizeof(ValueType));          // Buffer value (if applicable)

			// Allocate and initialize the buffer
			szBuffer = new char[nBufferSize + 1];
			memset(szBuffer, 0, nBufferSize + 1);

			size_t nOffset = 0;

			// 1. Serialize UID
			memcpy(szBuffer, &uidObjectType, sizeof(uint8_t));
			nOffset += sizeof(uint8_t);

			// 2. Serialize the number of keys
			memcpy(szBuffer + nOffset, &nKeyCount, sizeof(uint16_t));
			nOffset += sizeof(uint16_t);

			// 3. Serialize the pivot keys
			memcpy(szBuffer + nOffset, m_vtPivots.data(), nKeyCount * sizeof(KeyType));
			nOffset += nKeyCount * sizeof(KeyType);

			// 4. Serialize the child UIDs
			memcpy(szBuffer + nOffset, m_vtChildren.data(), nValueCount * sizeof(typename ObjectUIDType::NodeUID));
			nOffset += nValueCount * sizeof(typename ObjectUIDType::NodeUID);

			// 5. Serialize the number of buffer entries
			memcpy(szBuffer + nOffset, &nBufferCount, sizeof(uint32_t));
			nOffset += sizeof(uint32_t);

			// 6. Serialize each buffer entry
			for (const auto& entry : m_vtBuffer) 
			{
				// Serialize key
				memcpy(szBuffer + nOffset, &entry.first, sizeof(KeyType));
				nOffset += sizeof(KeyType);

				// Serialize operation
				uint8_t opRaw = static_cast<uint8_t>(entry.second.first);
				memcpy(szBuffer + nOffset, &opRaw, sizeof(uint8_t));
				nOffset += sizeof(uint8_t);

				// Serialize value (only for applicable operations)
				if (entry.second.first == Operations::Insert || entry.second.first == Operations::Update) 
				{
					memcpy(szBuffer + nOffset, &entry.second.second.value(), sizeof(ValueType));
					nOffset += sizeof(ValueType);
				}
			}
#ifdef __VALIDITY_CHECK__
			for (auto it = m_vtChildren.begin(); it != m_vtChildren.end(); it++)
			{
				assert((*it).getMediaType() >= 2);
			}
#endif //__VALIDITY_CHECK__
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement custom de/serializer.");
		}
	}

public:
	// Returns the number of keys (pivots) in the node
	inline size_t getKeysCount() const
	{
		return m_vtPivots.size();
	}

	// Finds the index of the child node for the given key
	inline size_t getChildNodeIdx(const KeyType& key) const
	{
		auto it = std::upper_bound(m_vtPivots.begin(), m_vtPivots.end(), key);
		return std::distance(m_vtPivots.begin(), it);
	}

	//  retrieve the correct pivot key during merging.
	inline KeyType getPivotForMerge(const ObjectUIDType& siblingUID) const {
		// Find the sibling's index
		auto it = std::find(m_vtChildren.begin(), m_vtChildren.end(), siblingUID);
		if (it == m_vtChildren.end()) {
			throw std::runtime_error("Sibling UID not found in children.");
		}

		size_t siblingIndex = std::distance(m_vtChildren.begin(), it);

		// The pivot key is just before the sibling in the pivots vector
		if (siblingIndex == 0) {
			throw std::runtime_error("Cannot get pivot for leftmost child.");
		}

		return m_vtPivots[siblingIndex - 1];
	}

	// Gets the child at the given index
	inline const ObjectUIDType& getChildAt(size_t nIdx) const
	{
		return m_vtChildren[nIdx];
	}

	// Gets the child node corresponding to the given key
	inline const ObjectUIDType& getChild(const KeyType& key) const
	{
		return m_vtChildren[getChildNodeIdx(key)];
	}

	// Returns the first pivot key
	inline const KeyType& getFirstChild() const
	{
		return m_vtPivots[0];
	}

	// TODO: Should be removed? For Testing
	void setBufferSize(size_t bufferSize)
	{
		m_nBufferSize = bufferSize;
	}

	// Checks if the node requires a split based on the degree
	inline bool requireSplit(size_t nDegree) const
	{
		return m_vtPivots.size() > nDegree;
	}

	inline bool canTriggerSplit(size_t nDegree) const
	{
		return m_vtPivots.size() + 1 > nDegree;
	}

	inline bool canTriggerMerge(size_t nDegree) const
	{
		return m_vtPivots.size() <= std::ceil(nDegree / 2.0f) + 1;	// TODO: macro!
	}

	inline bool requireMerge(size_t nDegree) const
	{
		return m_vtPivots.size() <= std::ceil(nDegree / 2.0f);
	}

	inline bool requireFlushBuffer() const
	{
		return m_vtBuffer.size() >= m_nBufferSize;
	}

	inline size_t getSize() const
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			return sizeof(uint8_t)                                                // UID
				+ sizeof(uint16_t)                                                // Total keys
				+ (m_vtPivots.size() * sizeof(KeyType))                           // Size of all Keys
				+ (m_vtChildren.size() * sizeof(typename ObjectUIDType::NodeUID)) // Size of all Values
				+ sizeof(uint32_t)                                                // Number of entries in Buffer
				+ (m_vtBuffer.size() * (sizeof(KeyType)                           // Buffer key
					+ sizeof(uint8_t)                            // Buffer operation
					+ sizeof(ValueType)));                       // Buffer value (if applicable)
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
	}

	// add a child node to the internal node
	inline void addChild(const ObjectUIDType& child)
	{
		m_vtChildren.push_back(child);
	}

	// remove a child node from the internal node
	inline void removeChild(size_t index)
	{
		if (index >= m_vtChildren.size())
		{
			throw std::out_of_range("Child index out of range");
		}
		m_vtChildren.erase(m_vtChildren.begin() + index);
	}

	// update child uid
	template <typename CacheObjectType>
#ifdef __TRACK_CACHE_FOOTPRINT__
	int32_t updateChildUID(std::shared_ptr<CacheObjectType> ptrChildNode, const ObjectUIDType& uidOld, const ObjectUIDType& uidNew)
#else //__TRACK_CACHE_FOOTPRINT__
	void updateChildUID(std::shared_ptr<CacheObjectType> ptrChildNode, const ObjectUIDType& uidOld, const ObjectUIDType& uidNew)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
		if (!ptrChildNode)
		{
			throw std::invalid_argument("ptrChildNode is null.");
		}
		const KeyType* key = nullptr;
		if (std::holds_alternative<std::shared_ptr<SelfType>>(ptrChildNode->getInnerData()))
		{
			std::shared_ptr<SelfType> ptrIndexNode = std::get<std::shared_ptr<SelfType>>(ptrChildNode->getInnerData());
			key = &ptrIndexNode->getFirstChild();
		}
		else
		{
			std::shared_ptr<DataNodeType> ptrDataNode = std::get<std::shared_ptr<DataNodeType>>(ptrChildNode->getInnerData());
			key = &ptrDataNode->getFirstChild();
		}

		// Locate the index of the child node
		auto it = std::upper_bound(m_vtPivots.begin(), m_vtPivots.end(), *key);
		auto index = std::distance(m_vtPivots.begin(), it);

		// Ensure the old UID matches the current UID at the index
		// instead of assert(m_vtChildren[index] == uidOld) -> throw exception
		if (m_vtChildren[index] != uidOld)
		{
			throw std::runtime_error("UID mismatch: Expected UID does not match the current child UID.");
		}

		// Update the UID
		m_vtChildren[index] = uidNew;
#ifdef __TRACK_CACHE_FOOTPRINT__
		return 0;
#else //__TRACK_CACHE_FOOTPRINT__
		return;
#endif //__TRACK_CACHE_FOOTPRINT__
	}

	template <typename CacheObjectType>
	bool updateChildrenUIDs(std::unordered_map<ObjectUIDType, std::pair<std::optional<ObjectUIDType>, std::shared_ptr<CacheObjectType>>>& mpUIDUpdates)
	{
		bool bDirty = false; // Tracks if any updates were made

		for (auto it = m_vtChildren.begin(), itend = m_vtChildren.end(); it != itend; ++it)
		{
			// Check if the current child UID exists in the update map
			auto mapIt = mpUIDUpdates.find(*it);
			if (mapIt != mpUIDUpdates.end())
			{
				ObjectUIDType uidOld = *it; // Store the old UID
				// Update the child UID with the new one
				if (mapIt->second.first.has_value())
				{
					*it = mapIt->second.first.value();
					bDirty = true; // Mark the node as dirty
				}

				// Remove the entry from the map to signify it's been processed
				mpUIDUpdates.erase(mapIt);
			}
		}

		return bDirty; // Return whether the node was modified
	}

	// the memory used by the node's components
	inline size_t getMemoryUsage() const
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			// todo: should we Subtracts the sizes of m_vtPivots, m_vtChildren, and m_vtBuffer from the size of the class as we only count the dynamic memory they allocate
			size_t nMemory = sizeof(*this);
			// Memory for pivots
			nMemory += m_vtPivots.size() * sizeof(KeyType);
			// Memory for children
			nMemory += m_vtChildren.size() * sizeof(ObjectUIDType);
			// Memory for buffer entries
			for (const auto& entry : m_vtBuffer)
			{
				nMemory += sizeof(KeyType);    // Key
				nMemory += sizeof(uint8_t);   // Operation
				if (entry.second.second.has_value())
				{
					nMemory += sizeof(ValueType); // Value (if present)
				}
			}
			return nMemory;
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate memory usage.");
		}
	}


	// insert buffered operations into the node
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode insertBuffered(const KeyType& key, const ValueType& value, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
		inline ErrorCode insertBuffered(const KeyType& key, const ValueType& value) 
#endif //__TRACK_CACHE_FOOTPRINT__
		{
#ifdef __TRACK_CACHE_FOOTPRINT__
			uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
			uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
			uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Locate the key in the buffer
		auto it = std::lower_bound(m_vtBuffer.begin(), m_vtBuffer.end(), key,
			[](const auto& entry, const KeyType& key) {
				return entry.first < key;
			});

		if (it != m_vtBuffer.end() && it->first == key) {
			// Merge operation if key already exists
			mergeOperations(it->second, { Operations::Insert, value });
		}
		else {
			// Add new operation to the buffer
			m_vtBuffer.insert(it, { key, { Operations::Insert, value } });
		}

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity())
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity())
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity())
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
			}
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT_

		// Check if the buffer is full and needs flushing
		if (m_vtBuffer.size() >= m_nBufferSize) {
#ifdef __TRACK_CACHE_FOOTPRINT__
			return flushBuffer(nMemoryFootprint);
#else //__TRACK_CACHE_FOOTPRINT__
			return flushBuffer();
#endif //__TRACK_CACHE_FOOTPRINT__
		}

		return ErrorCode::Success;
	}

	// Insert
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode insert(const KeyType & key, const std::pair<Operations, std::optional<ValueType>>& newOperation, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode insert(const KeyType& key, const std::pair<Operations, std::optional<ValueType>>& newOperation)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity= m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		auto it = std::lower_bound(m_vtBuffer.begin(), m_vtBuffer.end(), key, [](const std::pair<KeyType, std::pair<Operations, std::optional<ValueType>>>& entry, const KeyType& key) {
			return entry.first < key;
			});

		if (it != m_vtBuffer.end() && it->first == key) {
			// Merge the new operation with the existing one
			mergeOperations(it->second, newOperation);
		}
		else {
			// Insert the new key-operation pair
			m_vtBuffer.insert(it, { key, newOperation });
		}

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity())
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity())
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity())
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
			}
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__

		// Check buffer size and flush if necessary
		if (m_vtBuffer.size() >= m_nBufferSize) {
#ifdef __TRACK_CACHE_FOOTPRINT__
			return flushBuffer(nMemoryFootprint);
#else //__TRACK_CACHE_FOOTPRINT__
			return flushBuffer();
#endif //__TRACK_CACHE_FOOTPRINT__
		}
		return ErrorCode::Success;
	}

	// Update
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode update(const KeyType& key, const ValueType& newValue, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode update(const KeyType& key, const ValueType& newValue)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Find the key in the buffer
		auto it = std::lower_bound(m_vtBuffer.begin(), m_vtBuffer.end(), key, [](const auto& entry, const KeyType& key) {
			return entry.first < key;
			});

		// Key exists in the buffer
		if (it != m_vtBuffer.end() && it->first == key) {
			// Merge the new update operation
			mergeOperations(it->second, { Operations::Update, newValue });
			return ErrorCode::Success;
		}
		else {
			// Key not found; add as a new operation
			m_vtBuffer.insert(it, { key, {Operations::Update, newValue} });
#ifdef __TRACK_CACHE_FOOTPRINT__
			if constexpr (std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value)
			{
				if (nPivotContainerCapacity != m_vtPivots.capacity())
				{
					nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
					nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
				}

				if (nChildrenContainerCapacity != m_vtChildren.capacity())
				{
					nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
					nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
				}

				if (nBufferContainerCapacity != m_vtBuffer.capacity())
				{
					nMemoryFootprint -= nBufferContainerCapacity * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
					nMemoryFootprint += m_vtBuffer.capacity() * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
				}
			}
			else
			{
				static_assert(
					std::is_trivial<KeyType>::value &&
					std::is_standard_layout<KeyType>::value &&
					std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
					std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
					std::is_trivial<ValueType>::value &&
					std::is_standard_layout<ValueType>::value,
					"Non-POD type is provided. Kindly provide functionality to calculate size.");
			}
#endif //__TRACK_CACHE_FOOTPRINT__

			// Check buffer size
			if (m_vtBuffer.size() >= m_nBufferSize) {
#ifdef __TRACK_CACHE_FOOTPRINT__
				return flushBuffer(nMemoryFootprint);
#else //__TRACK_CACHE_FOOTPRINT__
				return flushBuffer();
#endif //__TRACK_CACHE_FOOTPRINT__
			}
			return ErrorCode::Success;
		}
	}

	// Remove
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode remove(const KeyType& key, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode remove(const KeyType& key) 
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Find the key in the buffer
		auto it = std::lower_bound(m_vtBuffer.begin(), m_vtBuffer.end(), key, [](const auto& entry, const KeyType& key) {
			return entry.first < key;
			});

		if (it != m_vtBuffer.end() && it->first == key) 
		{
			// If the key exists in the buffer
			switch (it->second.first) 
			{
			case Operations::Insert:
				// Remove the entry if it was an Insert
				m_vtBuffer.erase(it);
				break;
			case Operations::Update:
				// Replace Update with Delete
				it->second = { Operations::Delete, std::nullopt };
				break;
			case Operations::Delete:
				// Key is already marked for deletion, no action needed
				return ErrorCode::Success;
			}
		}
		else 
		{
			// If the key does not exist, add a Delete operation
			m_vtBuffer.insert(it, { key, {Operations::Delete, std::nullopt} });
		}
#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity())
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity())
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity())
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
			}
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__

		if (m_vtBuffer.size() >= m_nBufferSize) 
		{
#ifdef __TRACK_CACHE_FOOTPRINT__
			return flushBuffer(nMemoryFootprint);
#else //__TRACK_CACHE_FOOTPRINT__
			return flushBuffer();
#endif //__TRACK_CACHE_FOOTPRINT__
		}
		return ErrorCode::Success;
	}

	// Search
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode search(const KeyType& key, std::optional<ValueType>& value, int32_t& nMemoryFootprint) const
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode search(const KeyType& key, std::optional<ValueType>& value) const 
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Find the key in the buffer
		auto it = std::lower_bound(m_vtBuffer.begin(), m_vtBuffer.end(), key, [](const auto& entry, const KeyType& key) {
			return entry.first < key;
			});
		if (it != m_vtBuffer.end() && it->first == key) {
			// If the key exists in the buffer
			if (it->second.first == Operations::Delete) {
				// Key is marked for deletion
				return ErrorCode::KeyNotFound;
			}
			else if (it->second.first == Operations::Insert || it->second.first == Operations::Update) {
				// Key is marked for insertion or update
				value = it->second.second;
				return ErrorCode::Success;
			}
		}
#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity())
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity())
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity())
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
			}
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__
		else {
			// Key not found in the buffer
			size_t childIndex = getChildNodeIdx(key);
#ifdef __TRACK_CACHE_FOOTPRINT__
			return m_vtChildren[childIndex].search(key, value, nMemoryFootprint);
#else //__TRACK_CACHE_FOOTPRINT__
			return m_vtChildren[childIndex].search(key, value);
#endif //__TRACK_CACHE_FOOTPRINT__
		}
	}


	// merge two operations of the same key
	inline void mergeOperations(std::pair<Operations, std::optional<ValueType>>& existingOperation, const std::pair<Operations, std::optional<ValueType>>& newOperation)
	{
		switch (existingOperation.first) {
		case Operations::Insert:
			if (newOperation.first == Operations::Update) {
				existingOperation.second = newOperation.second;
			}
			else if (newOperation.first == Operations::Delete) {
				existingOperation = { Operations::Delete, std::nullopt };
			}
			break;

		case Operations::Update:
			if (newOperation.first == Operations::Update) {
				existingOperation.second = newOperation.second;
			}
			else if (newOperation.first == Operations::Delete) {
				existingOperation = { Operations::Delete, std::nullopt };
			}
			break;

		case Operations::Delete:
			break;

		default:
			throw std::runtime_error("Unexpected operation type in mergeOperations.");
		}
	}

	// flush the buffer
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode flushBuffer(int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode flushBuffer()
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__
		for (const auto& entry : m_vtBuffer) 
		{
			const KeyType& key = entry.first;
			const Operations& op = entry.second.first;
			const std::optional<ValueType>& value = entry.second.second;

			size_t childIndex = getChildNodeIdx(key);

			// Propagate the operation to the child node
			ErrorCode result = ErrorCode::Success;
			switch (op) 
			{
			case Operations::Insert:
				result = m_vtChildren[childIndex].insert(key, value.value());
				break;
			case Operations::Update:
				result = m_vtChildren[childIndex].update(key, value.value());
				break;
			case Operations::Delete:
				result = m_vtChildren[childIndex].remove(key);
				break;
			default:
				return ErrorCode::Error; // Unsupported operation
			}

			if (result != ErrorCode::Success) 
			{
				return result; // Return the first error encountered
			}
		}

		m_vtBuffer.clear(); // Clear the buffer after flushing
#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
			std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity())
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity())
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity())
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(std::pair<KeyType, std::pair<Operations, std::optional<ValueType>> >);
			}
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<typename ObjectUIDType::NodeUID>::value &&
				std::is_standard_layout<typename ObjectUIDType::NodeUID>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__
		return ErrorCode::Success;
	}

	// Split a child node
	template <typename CacheType, typename CacheObjectTypePtr>
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode split(std::shared_ptr<CacheType> ptrCache, std::optional<ObjectUIDType>& uidSibling, CacheObjectTypePtr& ptrSibling, KeyType& pivotKeyForParent, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode split(std::shared_ptr<CacheType> ptrCache, std::optional<ObjectUIDType>& uidSibling, CacheObjectTypePtr& ptrSibling, KeyType& pivotKeyForParent)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Determine the middle point
		size_t nMid = m_vtPivots.size() / 2;

		// Create a sibling node and move the right half of pivots and children
		ptrCache->template createObjectOfType<SelfType>(
			uidSibling, ptrSibling,
			m_vtPivots.begin() + nMid + 1, m_vtPivots.end(),
			m_vtChildren.begin() + nMid + 1, m_vtChildren.end()
		);

		if (!uidSibling) 
		{
			return ErrorCode::Error; // Failed to create sibling
		}

		// Promote the middle key to the parent
		pivotKeyForParent = m_vtPivots[nMid];

		// Adjust the current node's pivots and children
		m_vtPivots.resize(nMid);
		m_vtChildren.resize(nMid + 1);

		// Adjust the buffer: Distribute the buffer entries
		std::vector<std::pair<KeyType, std::pair<Operations, std::optional<ValueType>>>> bufferForSibling;
		for (const auto& entry : m_vtBuffer) 
		{
			if (entry.first > pivotKeyForParent) 
			{
				bufferForSibling.push_back(entry); // Move buffer entries to sibling
			}
		}
		// Remove these entries from the current node's buffer
		m_vtBuffer.erase(
			std::remove_if(m_vtBuffer.begin(), m_vtBuffer.end(), [&](const auto& entry) 
				{
				return entry.first > pivotKeyForParent;
				}),
			m_vtBuffer.end()
		);

		// Assign the buffer entries to the sibling node
		ptrSibling->m_vtBuffer = std::move(bufferForSibling);

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity()) 
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity()) 
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ValueType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity()) 
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(ValueType);
			}
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__

		return ErrorCode::Success;
	}

	// Move an entity from the left sibling
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromLHSSibling(std::shared_ptr<SelfType> ptrLHSSibling, KeyType& pivotKeyForEntity, KeyType& pivotKeyForParent, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromLHSSibling(std::shared_ptr<SelfType> ptrLHSSibling, KeyType& pivotKeyForEntity, KeyType& pivotKeyForParent)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();

		uint32_t nLHSPivotContainerCapacity = ptrLHSSibling->m_vtPivots.capacity();
		uint32_t nLHSChildrenContainerCapacity = ptrLHSSibling->m_vtChildren.capacity();
		uint32_t nLHSBufferContainerCapacity = ptrLHSSibling->m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Step 1: Move the largest pivot key and rightmost child from LHS sibling
		if (ptrLHSSibling->m_vtPivots.empty() || ptrLHSSibling->m_vtChildren.empty()) 
		{
			return ErrorCode::Error; // Invalid state for LHS sibling
		}


		KeyType key = ptrLHSSibling->m_vtPivots.back();
		ObjectUIDType value = ptrLHSSibling->m_vtChildren.back();

		ptrLHSSibling->m_vtPivots.pop_back();
		ptrLHSSibling->m_vtChildren.pop_back();

		assert(ptrLHSSibling->m_vtPivots.size() > 0); // Ensure LHS sibling still has valid pivots

		// Step 2: Add the parent's pivot key to this node and update parent's pivot key
		m_vtPivots.insert(m_vtPivots.begin(), pivotKeyForEntity);
		m_vtChildren.insert(m_vtChildren.begin(), value);

		pivotKeyForParent = key;

		// Step 3: Transfer relevant buffer entries from LHS sibling to this node
		for (auto it = ptrLHSSibling->m_vtBuffer.begin(); it != ptrLHSSibling->m_vtBuffer.end(); ) 
		{
			const auto& entry = *it;

			// Move only entries that affect the current node's range
			if (entry.first <= pivotKeyForEntity) 
			{
				m_vtBuffer.push_back(entry); // Transfer entry
				it = ptrLHSSibling->m_vtBuffer.erase(it); // Remove from LHS sibling
			}
			else 
			{
				++it; // Move to the next entry
			}
		}

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity()) 
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity()) 
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity()) 
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(ValueType);
			}

			if (nLHSPivotContainerCapacity != ptrLHSSibling->m_vtPivots.capacity()) 
			{
				nMemoryFootprint -= nLHSPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += ptrLHSSibling->m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nLHSChildrenContainerCapacity != ptrLHSSibling->m_vtChildren.capacity()) 
			{
				nMemoryFootprint -= nLHSChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += ptrLHSSibling->m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nLHSBufferContainerCapacity != ptrLHSSibling->m_vtBuffer.capacity()) 
			{
				nMemoryFootprint -= nLHSBufferContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += ptrLHSSibling->m_vtBuffer.capacity() * sizeof(ValueType);
			}
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__
	}

#ifdef __TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromRHSSibling(std::shared_ptr<SelfType> ptrRHSSibling, KeyType& pivotKeyForEntity, KeyType& pivotKeyForParent, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromRHSSibling(std::shared_ptr<SelfType> ptrRHSSibling, KeyType& pivotKeyForEntity, KeyType& pivotKeyForParent)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
		uint32_t nBufferContainerCapacity = m_vtBuffer.capacity();

		uint32_t nRHSPivotContainerCapacity = ptrRHSSibling->m_vtPivots.capacity();
		uint32_t nRHSChildrenContainerCapacity = ptrRHSSibling->m_vtChildren.capacity();
		uint32_t nRHSBufferContainerCapacity = ptrRHSSibling->m_vtBuffer.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Step 1: Move the largest pivot key and rightmost child from LHS sibling
		if (ptrRHSSibling->m_vtPivots.empty() || ptrRHSSibling->m_vtChildren.empty()) 
		{
			return ErrorCode::Error; // Invalid state for LHS sibling
		}

		// Step 1: Move the smallest pivot key and leftmost child from RHS sibling
		KeyType key = ptrRHSSibling->m_vtPivots.front();
		ObjectUIDType value = ptrRHSSibling->m_vtChildren.front();

		ptrRHSSibling->m_vtPivots.erase(ptrRHSSibling->m_vtPivots.begin());
		ptrRHSSibling->m_vtChildren.erase(ptrRHSSibling->m_vtChildren.begin());

		assert(ptrRHSSibling->m_vtPivots.size() > 0); // Ensure RHS sibling still has valid pivots

		// Step 2: Add the parent's pivot key to this node and update parent's pivot key
		m_vtPivots.push_back(pivotKeyForEntity);
		m_vtChildren.push_back(value);

		pivotKeyForParent = key;

		// Step 3: Transfer relevant buffer entries from RHS sibling to this node
		for (auto it = ptrRHSSibling->m_vtBuffer.begin(); it != ptrRHSSibling->m_vtBuffer.end();) 
		{
			const auto& entry = *it;

			// Move only entries that affect the current node's range
			if (entry.first <= key) 
			{
				m_vtBuffer.push_back(entry); // Transfer entry
				it = ptrRHSSibling->m_vtBuffer.erase(it); // Remove from RHS sibling
			}
			else 
			{
				break; // Stop early, as buffer entries are sorted by key
			}
		}

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity()) 
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity()) 
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nBufferContainerCapacity != m_vtBuffer.capacity()) 
			{
				nMemoryFootprint -= nBufferContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtBuffer.capacity() * sizeof(ValueType);
			}

			if (nRHSPivotContainerCapacity != ptrRHSSibling->m_vtPivots.capacity()) 
			{
				nMemoryFootprint -= nRHSPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += ptrRHSSibling->m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nRHSChildrenContainerCapacity != ptrRHSSibling->m_vtChildren.capacity()) 
			{
				nMemoryFootprint -= nRHSChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += ptrRHSSibling->m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}

			if (nRHSBufferContainerCapacity != ptrRHSSibling->m_vtBuffer.capacity()) 
			{
				nMemoryFootprint -= nRHSBufferContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += ptrRHSSibling->m_vtBuffer.capacity() * sizeof(ValueType);
			}
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif //__TRACK_CACHE_FOOTPRINT__
	}

	// Merge nodes 
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline void mergeNodes(std::shared_ptr<SelfType> ptrSibling, KeyType& pivotKey, bool isRightSibling, int32_t& nMemoryFootprint)
#else
	inline void mergeNodes(std::shared_ptr<SelfType> ptrSibling, KeyType& pivotKey, bool isRightSibling)
#endif
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nPivotContainerCapacity = m_vtPivots.capacity();
		uint32_t nChildrenContainerCapacity = m_vtChildren.capacity();
#endif

	
		// Merging with the right-hand sibling
		m_vtPivots.push_back(pivotKey); // Add pivot key from parent
		m_vtPivots.insert(m_vtPivots.end(), ptrSibling->m_vtPivots.begin(), ptrSibling->m_vtPivots.end());
		m_vtChildren.insert(m_vtChildren.end(), ptrSibling->m_vtChildren.begin(), ptrSibling->m_vtChildren.end());
		m_vtBuffer.insert(m_vtBuffer.end(), ptrSibling->m_vtBuffer.begin(), ptrSibling->m_vtBuffer.end());

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value) 
		{
			if (nPivotContainerCapacity != m_vtPivots.capacity()) 
			{
				nMemoryFootprint -= nPivotContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtPivots.capacity() * sizeof(KeyType);
			}

			if (nChildrenContainerCapacity != m_vtChildren.capacity()) 
			{
				nMemoryFootprint -= nChildrenContainerCapacity * sizeof(ObjectUIDType);
				nMemoryFootprint += m_vtChildren.capacity() * sizeof(ObjectUIDType);
			}
		}
		else 
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly provide functionality to calculate size.");
		}
#endif
	}

	void printBuffer() const {
		std::cout << "Buffer Contents:" << std::endl;
		for (const auto& entry : m_vtBuffer) 
		{
			std::cout << "Key: " << entry.first
				<< ", Operation: " << static_cast<int>(entry.second.first);
			if (entry.second.second.has_value()) 
			{
				std::cout << ", Value: " << entry.second.second.value();
			}
			std::cout << std::endl;
		}
	}
};
