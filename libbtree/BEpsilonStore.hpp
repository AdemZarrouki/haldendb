#pragma once
#include <memory>
#include <iostream>
#include <stack>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <syncstream>
#include <thread>
#include <cmath>
#include <exception>
#include <variant>
#include <unordered_map>
#include "CacheErrorCodes.h"
#include "ErrorCodes.h"
#include "VariadicNthType.h"
#include <tuple>
#include <vector>
#include <stdexcept>
#include <thread>
#include <iostream>
#include <fstream>
#include <assert.h>
#include <algorithm>

using namespace std::chrono_literals;

#ifdef __TREE_WITH_CACHE__
template <typename ICallback, typename KeyType, typename ValueType, typename CacheType>
class BEpsilonStore : public ICallback
#else //__TREE_WITH_CACHE__
template <typename KeyType, typename ValueType, typename CacheType>
class BEpsilonStore
#endif //__TREE_WITH_CACHE__
{
	typedef CacheType::ObjectUIDType ObjectUIDType;
	typedef CacheType::ObjectType ObjectType;
	typedef CacheType::ObjectTypePtr ObjectTypePtr;

    using DataNodeEpsilonType = typename std::tuple_element<0, typename ObjectType::ValueCoreTypesTuple>::type;
    using IndexNodeEpsilonType = typename std::tuple_element<1, typename ObjectType::ValueCoreTypesTuple>::type;

private:
    uint32_t m_nDegree;  // Tree degree
    uint32_t m_nBufferSize; // Maximum buffer size in internal nodes
    std::shared_ptr<CacheType> m_ptrCache;
    std::optional<ObjectUIDType> m_uidRootNode;

#ifdef __CONCURRENT__
	mutable std::shared_mutex m_mutex;
#endif //__CONCURRENT__

public:
	// Destructor
	~BEpsilonStore()
	{
		m_ptrCache.reset();
		m_uidRootNode.reset();
	}

	// Constructor
	template<typename... CacheArgs>
	BEpsilonStore(uint32_t nDegree, uint32_t nBufferSize, CacheArgs... args)
		: m_nDegree(nDegree)
		, m_nBufferSize(nBufferSize)
		, m_uidRootNode(std::nullopt)
	{
		m_ptrCache = std::make_shared<CacheType>(args...);
	}
	
	// Initialize the tree with an empty root node
	template <typename DefaultNodeType>
	void init() 
	{
#ifdef __TREE_WITH_CACHE__
		m_ptrCache->init(this);
#endif //__TREE_WITH_CACHE__
		m_ptrCache->template createObjectOfType<DefaultNodeType>(m_uidRootNode);
	}

	// insert a key
	inline ErrorCode insert(const KeyType& key, const ValueType& value) 
	{
		if (!m_uidRootNode) {
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		ObjectUIDType uidCurrentNode = *m_uidRootNode;
		ObjectTypePtr ptrCurrentNode = nullptr;

		do
		{
			m_ptrCache->getObject(uidCurrentNode, ptrCurrentNode);
			if (!ptrCurrentNode) {
				throw std::runtime_error("Cache returned a null object during insert.");
			}

			// check if the current node is an internal node
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());
				
				ErrorCode result = ptrIndexEpsilonNode->insertBuffered(key, value);

				if (!ptrIndexEpsilonNode->requireSplit(m_nDegree)) {
					return ErrorCode::Success;
				}

				if (result == ErrorCode::Success && ptrIndexEpsilonNode->requireSplit(m_nDegree)) {
					// Handle split at the root level
					handleRootSplit(ptrIndexEpsilonNode);
				}
				return result;
			}

			// check if the current node is an leaf node
			else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData());
				ErrorCode result = ptrDataEpsilonNode->insert(key, value);

				if (!ptrDataEpsilonNode->requireSplit(m_nDegree)) 
				{
					return ErrorCode::Success;
				}

				if (result == ErrorCode::Success && ptrDataEpsilonNode->requireSplit(m_nDegree)) 
				{
					// Handle split at the root level
					handleRootSplit(ptrDataEpsilonNode);
				}
				return result;
			}
			else
			{
				throw std::runtime_error("Invalid node type.");
			}

		} while(true);
	}

	// handle split at the root level
	template <typename NodeType>
	void handleRootSplit(std::shared_ptr<NodeType> ptrRootNode) {
		// Create a new root node
		std::shared_ptr<IndexNodeEpsilonType> newRootNode;
		std::optional<ObjectUIDType> newRootUID;
		m_ptrCache->template createObjectOfType<IndexNodeEpsilonType>(newRootUID, newRootNode);

		if (!newRootUID || !newRootNode) {
			throw std::runtime_error("Failed to create a new root node.");
		}

		// Variables to hold sibling information and pivot key
		std::optional<ObjectUIDType> siblingUID;
		ObjectTypePtr siblingNode;
		KeyType pivotKey;

		// Split the current root node
		ErrorCode splitResult = ptrRootNode->split(m_ptrCache, siblingUID, siblingNode, pivotKey);

		if (splitResult != ErrorCode::Success || !siblingUID || !siblingNode) {
			throw std::runtime_error("Root node split failed.");
		}

		// Set up the new root node
		newRootNode->addChild(m_uidRootNode.value());
		newRootNode->addChild(siblingUID.value());

		// Insert the pivot key into the new root
		ErrorCode insertResult = newRootNode->insert(pivotKey, { Operations::Insert, std::nullopt });
		if (insertResult != ErrorCode::Success) {
			throw std::runtime_error("Failed to insert pivot key into the new root node.");
		}

		// Update the root node pointer
		m_uidRootNode = newRootUID;
	}

	// Search for a key
	inline ErrorCode search(const KeyType& key, ValueType& result) {
		if (!m_uidRootNode) {
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		ObjectUIDType uidCurrentNode = *m_uidRootNode;  // Start at the root node
		ObjectTypePtr ptrCurrentNode = nullptr;        // Current node pointer
		std::vector<std::pair<Operations, std::optional<ValueType>>> collectedMessages;  // Collected operations for the key

		while (true) 
		{
			// Load the current node from the cache
			m_ptrCache->getObject(uidCurrentNode, ptrCurrentNode);

			if (!ptrCurrentNode) {
				throw std::runtime_error("Cache returned a null object during search.");
			}

			// check if the current node is an internal node
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());
				// Collect messages from the buffer for the queried key
				auto buffer = ptrIndexEpsilonNode->getBuffer();
				for (const auto& message : buffer) {
					if (message.first == key) {
						collectedMessages.push_back(message.second);  // Collect operations for key
					}
				}

				uidCurrentNode = ptrIndexEpsilonNode->getChild(key);
				
			}

			// check if the current node is an leaf node
			else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Look for the key in the leaf node
				std::optional<ValueType> leafValue;
				ErrorCode resultCode = ptrDataEpsilonNode->getValue(key, leafValue);
				if (resultCode != ErrorCode::Success) 
				{
					return resultCode;  // Key not found
				}

				// Apply collected messages in reverse order
				for (auto it = collectedMessages.rbegin(); it != collectedMessages.rend(); ++it) {
					const auto& operation = *it;
					switch (operation.first) {
					case Operations::Insert:
					case Operations::Update:
						leafValue = operation.second;  // Apply insert or update
						break;
					case Operations::Delete:
						return ErrorCode::KeyNotFound;  // Key was deleted
					default:
						return ErrorCode::Error;  // Unsupported operation
					}
				}
				// Set the result to the final value
				if (leafValue.has_value()) {
					result = leafValue.value();
					return ErrorCode::Success;
				}
				else {
					return ErrorCode::KeyNotFound;  // Key does not exist
				}
			}
			else {
				throw std::runtime_error("Invalid node type encountered during search.");
			}
		}
	}


	inline ErrorCode remove(const KeyType& key) 
	{
		if (!m_uidRootNode) {
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		ObjectUIDType uidCurrentNode = *m_uidRootNode;
		ObjectTypePtr ptrCurrentNode = nullptr;

		while (true) {
			// Retrieve the current node
			m_ptrCache->getObject(uidCurrentNode, ptrCurrentNode);
			if (!ptrCurrentNode) {
				throw std::runtime_error("Failed to retrieve node during remove operation.");
			}

			// Check if the current node is an internal node
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData())) {
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Insert a buffered delete operation
				ptrIndexEpsilonNode->insert(key, { Operations::Delete, std::nullopt });

				// Check if the buffer needs flushing
				if (ptrIndexEpsilonNode->requireFlushBuffer()) {
					ptrIndexEpsilonNode->flushBuffer();
				}

				if (ptrIndexEpsilonNode->requireMerge(m_nDegree)) {
					// Locate siblings
					ObjectUIDType leftSiblingUID, rightSiblingUID;
					getSiblings(ptrIndexEpsilonNode, uidCurrentNode, leftSiblingUID, rightSiblingUID);

					if (leftSiblingUID) {
						ObjectTypePtr leftSiblingNode = nullptr;
						m_ptrCache->getObject(leftSiblingUID, leftSiblingNode);
						auto leftSibling = std::get<std::shared_ptr<IndexNodeEpsilonType>>(leftSiblingNode->getInnerData());

						// Merge with left sibling
						KeyType pivotKey = ptrIndexEpsilonNode->getPivotForMerge(leftSiblingUID);
						ptrIndexEpsilonNode->mergeNodes(leftSibling, pivotKey, false);
					}
					else if (rightSiblingUID) {
						ObjectTypePtr rightSiblingNode = nullptr;
						m_ptrCache->getObject(rightSiblingUID, rightSiblingNode);
						auto rightSibling = std::get<std::shared_ptr<IndexNodeEpsilonType>>(rightSiblingNode->getInnerData());

						// Merge with right sibling
						KeyType pivotKey = ptrIndexEpsilonNode->getPivotForMerge(rightSiblingUID);
						ptrIndexEpsilonNode->mergeNodes(rightSibling, pivotKey, true);
					}
				}

				// Traverse to the child node
				size_t childIndex = ptrIndexEpsilonNode->getChildNodeIdx(key);
				uidCurrentNode = ptrIndexEpsilonNode->getChildAt(childIndex);
			}
			// Check if the current node is a leaf node
			else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Remove the key directly from the leaf node
				ErrorCode result = ptrDataEpsilonNode->remove(key);

				// Check if the leaf node requires merging
				if (ptrDataEpsilonNode->requireMerge(m_nDegree)) {
					// Locate siblings
					ObjectUIDType leftSiblingUID, rightSiblingUID;
					getSiblings(ptrIndexNode, uidCurrentNode, leftSiblingUID, rightSiblingUID);

					if (leftSiblingUID) {
						ObjectTypePtr leftSiblingNode = nullptr;
						m_ptrCache->getObject(leftSiblingUID, leftSiblingNode);
						auto leftSibling = std::get<std::shared_ptr<DataNodeEpsilonType>>(leftSiblingNode->getInnerData());

						// Merge with left sibling
						ptrDataEpsilonNode->mergeNode(leftSibling);
					}
					else if (rightSiblingUID) {
						ObjectTypePtr rightSiblingNode = nullptr;
						m_ptrCache->getObject(rightSiblingUID, rightSiblingNode);
						auto rightSibling = std::get<std::shared_ptr<DataNodeEpsilonType>>(rightSiblingNode->getInnerData());

						// Merge with right sibling
						ptrDataEpsilonNode->mergeNode(rightSibling);
					}
				}

				return result;
			}
			else {
				throw std::runtime_error("Invalid node type encountered during remove operation.");
			}
		}
	}

	inline void getSiblings(
		const std::shared_ptr<IndexNodeEpsilonType>& ptrParentNode,
		const ObjectUIDType& uidNode,
		std::optional<ObjectUIDType>& leftSiblingUID,
		std::optional<ObjectUIDType>& rightSiblingUID) 
	{
		// todo: check getChildren() function
		const auto& children = ptrParentNode->getChildren(); // Retrieve the children vector
		auto it = std::find(children.begin(), children.end(), uidNode);

		if (it == children.end()) {
			throw std::runtime_error("Node UID not found in parent's children.");
		}

		size_t index = std::distance(children.begin(), it);

		// Determine left sibling
		if (index > 0) {
			leftSiblingUID = children[index - 1];
		}
		else {
			leftSiblingUID = std::nullopt; // No left sibling
		}

		// Determine right sibling
		if (index < children.size() - 1) {
			rightSiblingUID = children[index + 1];
		}
		else {
			rightSiblingUID = std::nullopt; // No right sibling
		}
	}


	// another insert method
	inline ErrorCode insertRecursive(const KeyType& key, const ValueType& value) {
		if (!m_uidRootNode) {
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		while (true) {
			ObjectUIDType uidCurrentNode = *m_uidRootNode;
			ObjectTypePtr ptrCurrentNode = nullptr;

			// Fetch the current root node
			m_ptrCache->getObject(uidCurrentNode, ptrCurrentNode);
			if (!ptrCurrentNode) {
				throw std::runtime_error("Cache returned a null object during insert.");
			}

			// Step 1: Handle root node splits
			if (shouldSplitRoot(ptrCurrentNode)) {
				handleRootSplit(ptrCurrentNode);
				continue;  // Restart the insertion process
			}

			// Step 2: Flush root buffer if necessary
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData())) {
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());
				if (ptrIndexEpsilonNode->requireFlushBuffer())
				{
					ptrIndexEpsilonNode->flushBuffer();
					if (ptrIndexEpsilonNode->getChildrenCount() == 1) {
						m_uidRootNode = ptrIndexEpsilonNode->getChildAt(0);
					}
					continue;  // Restart the insertion process
				}
				continue;  // Restart the insertion process
			}

			// Step 3: Insert the message
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData())) {
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());
				ErrorCode result = ptrIndexEpsilonNode->insertBuffered(key, value);
				if (result == ErrorCode::Success) {
					return result;  // Successfully buffered the operation
				}
			}
			else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData())) {
				auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData());
				return ptrDataEpsilonNode->insert(key, value);  // Apply directly to the leaf
			}
			else {
				throw std::runtime_error("Invalid node type.");
			}
		}
	}

	inline bool shouldSplitRoot(const ObjectTypePtr& ptrRootNode) const {
		if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrRootNode->getInnerData())) {
			auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrRootNode->getInnerData());
			return ptrIndexEpsilonNode->requireSplit(m_nDegree);
		}
		else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrRootNode->getInnerData())) {
			auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrRootNode->getInnerData());
			return ptrDataEpsilonNode->requireSplit(m_nDegree);
		}
		return false;
	}

	// range query search
	inline ErrorCode rangeQuery(const KeyType& firstKey, const KeyType& secondKey, std::vector<std::pair<KeyType, ValueType>>& results)
	{
		if (!m_uidRootNode) 
		{
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		ObjectUIDType uidCurrentNode = *m_uidRootNode;
		ObjectTypePtr ptrCurrentNode = nullptr;

		do {
			// Retrieve the current node
			m_ptrCache->getObject(uidCurrentNode, ptrCurrentNode);
			if (!ptrCurrentNode) 
			{
				throw std::runtime_error("Cache returned a null object during range query.");
			}

			// Check if the current node is an internal node
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Collect messages for the range
				const auto& buffer = ptrIndexEpsilonNode->getBuffer();
				for (const auto& message : buffer) 
				{
					if (message.first >= firstKey && message.first <= secondKey) 
					{
						if (message.second.first == Operations::Insert || message.second.first == Operations::Update)
						{
							results.push_back({ message.first, message.second.second.value() });
						}
					}
				}

				// Determine the next child to descend into
				uidCurrentNode = ptrIndexEpsilonNode->getChild(firstKey);
			}
			// Check if the current node is a leaf node
			else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData()))
			{
				auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Collect all key-value pairs in the range
				auto keys = ptrDataEpsilonNode->getKeys();
				auto values = ptrDataEpsilonNode->getValues();
				for (size_t i = 0; i < keys.size(); ++i) 
				{
					if (keys[i] >= firstKey && keys[i] <= secondKey) 
					{
						results.push_back({ keys[i], values[i] });
					}
				}

				break; // No more nodes to traverse
			}
			else 
			{
				throw std::runtime_error("Invalid node type encountered during range query.");
			}
		} while (true);

		return ErrorCode::Success;
	}

	// bulk insert
	inline ErrorCode bulkInsert(const std::vector<std::pair<KeyType, ValueType>>& data)
	{
		if (!m_uidRootNode)
		{
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		if (data.empty()) {
			// todo: check if this is correct or we need to return an error
			return ErrorCode::Success;
		}

		auto sortedData = data; // Make a copy to avoid modifying the input
		std::sort(sortedData.begin(), sortedData.end(),
			[](const auto& a, const auto& b) { return a.first < b.first; });

		for (const auto& [key, value] : sortedData) {
			ErrorCode result = insert(key, value);
			if (result != ErrorCode::Success) {
				return result; // Return immediately if an insertion fails
			}
		}
		return ErrorCode::Success;
	}

	// update tje value of a key
	inline ErrorCode update(const KeyType& key, const ValueType& newValue)
	{
		if (!m_uidRootNode) {
			return ErrorCode::TreeEmpty; // Handle the case where the tree is empty
		}

		ObjectUIDType uidCurrentNode = *m_uidRootNode;
		ObjectTypePtr ptrCurrentNode = nullptr;

		do 
		{
			// Retrieve the current node
			m_ptrCache->getObject(uidCurrentNode, ptrCurrentNode);
			if (!ptrCurrentNode) 
			{
				throw std::runtime_error("Cache returned a null object during update.");
			}

			// Check if the current node is an internal node
			if (std::holds_alternative<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData())) 
			{
				auto ptrIndexEpsilonNode = std::get<std::shared_ptr<IndexNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Buffer the update operation
				ptrIndexEpsilonNode->insert(key, { Operations::Update, newValue });

				// Check if the buffer needs flushing
				if (ptrIndexEpsilonNode->requireFlushBuffer()) 
				{
					ptrIndexEpsilonNode->flushBuffer();
				}

				// Traverse to the child node
				uidCurrentNode = ptrIndexEpsilonNode->getChild(key);
			}
			// Check if the current node is a leaf node
			else if (std::holds_alternative<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData())) 
			{
				auto ptrDataEpsilonNode = std::get<std::shared_ptr<DataNodeEpsilonType>>(ptrCurrentNode->getInnerData());

				// Update the key directly in the leaf node
				ErrorCode result = ptrDataEpsilonNode->update(key, newValue);

				return result; // Return the result of the update
			}
			else 
			{
				throw std::runtime_error("Invalid node type encountered during update.");
			}
		} while (true);
	}
};
