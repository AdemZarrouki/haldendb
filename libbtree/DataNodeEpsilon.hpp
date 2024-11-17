#pragma once
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <optional>
#include <iostream>
#include <fstream>
#include <assert.h>
#include "ErrorCodes.h"


template <typename KeyType, typename ValueType, typename ObjectUIDType, uint8_t TYPE_UID>
class DataNodeEpsilon
{
public:
	// Unique identifier for the type of this node
	static const uint8_t UID = TYPE_UID;

private:
	struct Operation {
		enum Type { INSERT, DELETE, UPDATE, SEARCH } type;
		KeyType key;
		std::optional<ValueType> value;  // Only used for insertions
	};

	typedef DataNodeEpsilon<KeyType, ValueType, ObjectUIDType, TYPE_UID> SelfType;

	// Aliases for iterators over key and value vectors
	typedef std::vector<KeyType>::const_iterator KeyTypeIterator;
	typedef std::vector<ValueType>::const_iterator ValueTypeIterator;

	// Vectors to hold keys and corresponding values
	std::vector<KeyType> m_vtKeys;
	std::vector<ValueType> m_vtValues;

public:
	// Destructor: Clears the keys and values vectors
	~DataNodeEpsilon()
	{
		m_vtKeys.clear();
		m_vtValues.clear();
	}

	// Default constructor
	DataNodeEpsilon()
	{
	}

	// Copy constructor: Copies keys and values from another DataNodeEpsilon instance
	DataNodeEpsilon(const DataNodeEpsilon& source)
	{
		m_vtKeys.assign(source.m_vtKeys.begin(), source.m_vtKeys.end());
		m_vtValues.assign(source.m_vtValues.begin(), source.m_vtValues.end());
	}

	// Constructor that deserializes data from a char array (for POD types)
	DataNodeEpsilon(const char* szData)
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			uint16_t nTotalEntries = 0;
			// Copy the total number of entries
			std::memcpy(&nTotalEntries, szData, sizeof(uint16_t));
			szData += sizeof(uint16_t);
			// Copy the keys
			m_vtKeys.resize(nTotalEntries);
			std::memcpy(m_vtKeys.data(), szData, nTotalEntries * sizeof(KeyType));
			szData += nTotalEntries * sizeof(KeyType);
			// Copy the values
			m_vtValues.resize(nTotalEntries);
			std::memcpy(m_vtValues.data(), szData, nTotalEntries * sizeof(ValueType));
		}
		else
		{
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement custome de/serializer.");
		}

		// Constructor that initializes DataNodeEpsilon with iterators over keys and values
		DataNodeEpsilon(KeyTypeIterator itBeginKeys, KeyTypeIterator itEndKeys, ValueTypeIterator itBeginValues, ValueTypeIterator itEndValues)
		{
			m_vtKeys.resize(std::distance(itBeginKeys, itEndKeys));
			m_vtValues.resize(std::distance(itBeginValues, itEndValues));

			std::move(itBeginKeys, itEndKeys, m_vtKeys.begin());
			std::move(itBeginValues, itEndValues, m_vtValues.begin());
			//m_vtKeys.assign(itBeginKeys, itEndKeys);
			//m_vtValues.assign(itBeginValues, itEndValues);
		}
	}

public:
	// Serializes the node's data into a char buffer
	inline void serialize(char*& szBuffer, uint8_t& uidObjectType, uint32_t& nBufferSize) const {
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value) {
			uidObjectType = UID;
			uint16_t nTotalEntries = m_vtKeys.size();
			nBufferSize = sizeof(uint8_t) + sizeof(uint16_t) +
				(nTotalEntries * sizeof(KeyType)) +
				(nTotalEntries * sizeof(ValueType));

			// Use std::unique_ptr for safe memory management
			std::unique_ptr<char[]> buffer(new char[nBufferSize + 1]);
			memset(buffer.get(), 0, nBufferSize + 1);

			uint16_t nOffset = 0;
			memcpy(buffer.get(), &UID, sizeof(uint8_t));
			nOffset += sizeof(uint8_t);

			memcpy(buffer.get() + nOffset, &nTotalEntries, sizeof(uint16_t));
			nOffset += sizeof(uint16_t);

			uint16_t nKeysSize = nTotalEntries * sizeof(KeyType);
			memcpy(buffer.get() + nOffset, m_vtKeys.data(), nKeysSize);
			nOffset += nKeysSize;

			uint16_t nValuesSize = nTotalEntries * sizeof(ValueType);
			memcpy(buffer.get() + nOffset, m_vtValues.data(), nValuesSize);
			nOffset += nValuesSize;

			// Transfer ownership of the buffer to the caller
			szBuffer = buffer.release();
		}
		else {
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement custom de/serializer.");
		}
	}

	inline void writeToStream(std::ofstream& os) const {
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value) {
			uint16_t nTotalEntries = m_vtKeys.size();

			os.write(reinterpret_cast<const char*>(&nTotalEntries), sizeof(uint16_t));
			os.write(reinterpret_cast<const char*>(m_vtKeys.data()), nTotalEntries * sizeof(KeyType));
			os.write(reinterpret_cast<const char*>(m_vtValues.data()), nTotalEntries * sizeof(ValueType));
		}
		else {
			static_assert(
				std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value,
				"Non-POD type is provided. Kindly implement custom de/serializer.");
		}
	}

public:
	// Determines if the node requires a split based on the given degree
	inline bool requireSplit(size_t nDegree) const {
		return m_vtkeys.size() > nDegree;
	}

	// Determines if the node requires merging based on the given degree
	inline bool requireMerge(size_t nDegree) const {
		return m_vtKeys.size() < nDegree / 2;
	}

	// Retrieves the first key in the node
	inline const KeyType& getFirstChild() const {
		if (m_vtKeys.empty()) {
			throw std::runtime_error("DataNodeEpsilon::getFirstChild: Leaf Node is empty");
		}
		return m_vtKeys[0];
	}

	// Returns the number of keys in the node
	inline size_t getKeysCount() const
	{
		return m_vtKeys.size();
	}

	// Retrieves the value for a given key
	inline ErrorCode getValue(const KeyType& key, ValueType& value) const
	{
		auto it = std::lower_bound(m_vtKeys.begin(), m_vtKeys.end(), key);
		if (it != m_vtKeys.end() && *it == key)
		{
			size_t index = std::distance(m_vtKeys.begin(), it);
			value = m_vtValues[index];

			return ErrorCode::Success;
		}
		return ErrorCode::KeyDoesNotExist;
	}

	// Returns the size of the serialized node
	inline size_t getSize() const
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			return sizeof(uint8_t)
				+ sizeof(uint16_t)
				+ (m_vtKeys.size() * sizeof(KeyType))
				+ (m_vtValues.size() * sizeof(ValueType));
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
	}

	inline size_t getMemoryFootprint() const
	{
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			return
				sizeof(*this)
				+ (m_vtKeys.capacity() * sizeof(KeyType))
				+ (m_vtValues.capacity() * sizeof(ValueType));
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
	}

	// Removes a key-value pair from the node
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode remove(const KeyType& key, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode remove(const KeyType& key)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
		auto it = std::lower_bound(m_vtKeys.begin(), m_vtKeys.end(), key);

		if (it != m_vtKeys.end() && *it == key) {
#ifdef __TRACK_CACHE_FOOTPRINT__
			uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
			uint32_t nValueContainerCapacity = m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT_

			size_t index = std::distance(m_vtKeys.begin(), it);
			m_vtKeys.erase(it);
			m_vtValues.erase(m_vtValues.begin() + index);

#ifdef __TRACK_CACHE_FOOTPRINT__
			if constexpr (std::is_trivial<KeyType>::value &&
				std::is_standard_layout<KeyType>::value &&
				std::is_trivial<ValueType>::value &&
				std::is_standard_layout<ValueType>::value)
			{
				if (nKeyContainerCapacity != m_vtKeys.capacity())
				{
					nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
					nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
				}

				if (nValueContainerCapacity != m_vtValues.capacity())
				{
					nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
					nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
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
		else {
			return ErrorCode::KeyDoesNotExist;
		}
	}

#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode insert(const KeyType& key, const ValueType& value, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode insert(const KeyType& key, const ValueType& value)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
		uint32_t nValueContainerCapacity = m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Find the insertion position using binary search
		auto it = std::lower_bound(m_keys.begin(), m_keys.end(), key);

		/* TODO: do we need this?
		* If the key already exists, return an error
		if (it != m_keys.end() && *it == key) {
			return ErrorCode::KeyAlreadyExists;
		}
		*/

		// Insert the key and value at the correct position
		m_vtKeys.insert(it, key);
		m_vtValues.insert(it, value);

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nKeyContainerCapacity != m_vtKeys.capacity())
			{
				nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nValueContainerCapacity != m_vtValues.capacity())
			{
				nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
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

	// insert batch of keys and values
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode bulkInsert(const std::vector<KeyType>& keys, const std::vector<ValueType>& values, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode bulkInsert(const std::vector<KeyType>& keys, const std::vector<ValueType>& values)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
		uint32_t nValueContainerCapacity = m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		if (keys.size() != values.size()) {
			return ErrorCode::Error;
		}

		for (size_t i = 0; i < keys.size(); ++i) {
			// Find the insertion position using binary search
			auto it = std::lower_bound(m_vtKeys.begin(), m_vtKeys.end(), keys[i]);

			// Insert the key-value pair at the appropriate position
			m_vtKeys.insert(it, keys[i]);
			m_vtValues.insert(it, values[i]);
		}

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nKeyContainerCapacity != m_vtKeys.capacity())
			{
				nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nValueContainerCapacity != m_vtValues.capacity())
			{
				nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
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

	// Splits the node into two nodes and returns the pivot key for the parent node
	template <typename CacheType, typename CacheObjectTypePtr>
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline ErrorCode split(std::shared_ptr<CacheType>& ptrCache, std::optional<ObjectUIDType>& uidSibling, CacheObjectTypePtr& ptrSibling, KeyType& pivotKeyForParent, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline ErrorCode split(std::shared_ptr<CacheType>& ptrCache, std::optional<ObjectUIDType>& uidSibling, CacheObjectTypePtr& ptrSibling, KeyType& pivotKeyForParent)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
		uint32_t nValueContainerCapacity = m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		size_t nMid = m_vtKeys.size() / 2;
		ptrCache->template createObjectOfType<SelfType>(uidSibling, ptrSibling,
			m_vtKeys.begin() + nMid, m_vtKeys.end(),
			//TODO CHECK m_vtKeys.begin() + nMid + 1, m_vtKeys.end(),  should pivotKeyForParent be excluded as it is passed the parent node
			m_vtValues.begin() + nMid, m_vtValues.end());
		//TODO CHECK m_vtValues.begin() + nMid + 1, m_vtValues.end());  should pivotKeyForParent be excluded as it is passed the parent node

		if (!uidSibling)
		{
			return ErrorCode::Error;
		}

		pivotKeyForParent = m_vtKeys[nMid];

		m_vtKeys.resize(nMid);
		m_vtValues.resize(nMid);
#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nKeyContainerCapacity != m_vtKeys.capacity())
			{
				nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nValueContainerCapacity != m_vtValues.capacity())
			{
				nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
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

	//Moves an entity from left-hand sibling to the current node
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromLHSSibling(std::shared_ptr<SelfType> ptrLHSSibling, KeyType& pivotKeyForParent, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromLHSSibling(std::shared_ptr<SelfType> ptrLHSSibling, KeyType& pivotKeyForParent)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
		uint32_t nValueContainerCapacity = m_vtValues.capacity();

		uint32_t nLHSKeyContainerCapacity = ptrLHSSibling->m_vtKeys.capacity();
		uint32_t nLHSValueContainerCapacity = ptrLHSSibling->m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Check if the LHS sibling is valid and has at least one key
		if (!ptrLHSSibling || ptrLHSSibling->m_vtKeys.empty()) {
			throw std::runtime_error("Invalid operation: LHS sibling is null or empty.");
		}

		KeyType key = ptrLHSSibling->m_vtKeys.back();
		ValueType value = ptrLHSSibling->m_vtValues.back();

		ptrLHSSibling->m_vtKeys.pop_back();
		ptrLHSSibling->m_vtValues.pop_back();

		assert(ptrLHSSibling->m_vtKeys.size() > 0);

		pivotKeyForParent = key;  // or pivotKeyForParent = m_vtKeys.front(); ?

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nKeyContainerCapacity != m_vtKeys.capacity())
			{
				nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nValueContainerCapacity != m_vtValues.capacity())
			{
				nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
			}

			if (nLHSKeyContainerCapacity != ptrLHSSibling->m_vtKeys.capacity())
			{
				nMemoryFootprint -= nLHSKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += ptrLHSSibling->m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nLHSValueContainerCapacity != ptrLHSSibling->m_vtValues.capacity())
			{
				nMemoryFootprint -= nLHSValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += ptrLHSSibling->m_vtValues.capacity() * sizeof(ValueType);
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

	//Moves an entity from right-hand sibling to the current node
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromRHSSibling(std::shared_ptr<SelfType> ptrRHSSibling, KeyType& pivotKeyForParent, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline void moveAnEntityFromRHSSibling(std::shared_ptr<SelfType> ptrRHSSibling, KeyType& pivotKeyForParent)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
		uint32_t nValueContainerCapacity = m_vtValues.capacity();

		uint32_t nRHSKeyContainerCapacity = ptrRHSSibling->m_vtKeys.capacity();
		uint32_t nRHSValueContainerCapacity = ptrRHSSibling->m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		// Check if the LHS sibling is valid and has at least one key
		if (!ptrRHSSibling || ptrRHSSibling->m_vtKeys.empty()) {
			throw std::runtime_error("Invalid operation: RHS sibling is null or empty.");
		}

		KeyType key = ptrRHSSibling->m_vtKeys.front();
		ValueType value = ptrRHSSibling->m_vtValues.front();

		ptrRHSSibling->m_vtKeys.erase(ptrRHSSibling->m_vtKeys.begin());
		ptrRHSSibling->m_vtValues.erase(ptrRHSSibling->m_vtValues.begin());

		assert(ptrRHSSibling->m_vtKeys.size() > 0);

		m_vtKeys.push_back(key);
		m_vtValues.push_back(value);

		pivotKeyForParent = ptrRHSSibling->m_vtKeys.front();

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nKeyContainerCapacity != m_vtKeys.capacity())
			{
				nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nValueContainerCapacity != m_vtValues.capacity())
			{
				nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
			}

			if (nRHSKeyContainerCapacity != ptrRHSSibling->m_vtKeys.capacity())
			{
				nMemoryFootprint -= nRHSKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += ptrRHSSibling->m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nRHSValueContainerCapacity != ptrRHSSibling->m_vtValues.capacity())
			{
				nMemoryFootprint -= nRHSValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += ptrRHSSibling->m_vtValues.capacity() * sizeof(ValueType);
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

	// Merges the sibling node with the current node
#ifdef __TRACK_CACHE_FOOTPRINT__
	inline void mergeNode(std::shared_ptr<SelfType> ptrSibling, int32_t& nMemoryFootprint)
#else //__TRACK_CACHE_FOOTPRINT__
	inline void mergeNode(std::shared_ptr<SelfType> ptrSibling)
#endif //__TRACK_CACHE_FOOTPRINT__
	{
#ifdef __TRACK_CACHE_FOOTPRINT__
		uint32_t nKeyContainerCapacity = m_vtKeys.capacity();
		uint32_t nValueContainerCapacity = m_vtValues.capacity();
#endif //__TRACK_CACHE_FOOTPRINT__

		if (!ptrSibling) {
			throw std::runtime_error("Invalid operation: Sibling node is null.");
		}

		m_vtKeys.insert(m_vtKeys.end(), ptrSibling->m_vtKeys.begin(), ptrSibling->m_vtKeys.end());
		m_vtValues.insert(m_vtValues.end(), ptrSibling->m_vtValues.begin(), ptrSibling->m_vtValues.end());

#ifdef __TRACK_CACHE_FOOTPRINT__
		if constexpr (std::is_trivial<KeyType>::value &&
			std::is_standard_layout<KeyType>::value &&
			std::is_trivial<ValueType>::value &&
			std::is_standard_layout<ValueType>::value)
		{
			if (nKeyContainerCapacity != m_vtKeys.capacity())
			{
				nMemoryFootprint -= nKeyContainerCapacity * sizeof(KeyType);
				nMemoryFootprint += m_vtKeys.capacity() * sizeof(KeyType);
			}

			if (nValueContainerCapacity != m_vtValues.capacity())
			{
				nMemoryFootprint -= nValueContainerCapacity * sizeof(ValueType);
				nMemoryFootprint += m_vtValues.capacity() * sizeof(ValueType);
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

public:
	void print(std::ofstream& outFile) const {
		// Check if the file stream is open
		if (!outFile.is_open()) {
			throw std::runtime_error("Output file stream is not open.");
		}

		outFile << "DataNodeEpsilon: ";
		for (size_t i = 0; i < m_vtKeys.size(); ++i) {
			outFile << "Key: " << m_vtKeys[i] << ", Value: " << m_vtValues[i] << "\n";
		}
		outFile << std::endl;
	}
};