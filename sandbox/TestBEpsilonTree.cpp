#include <iostream>
#include <vector>
#include <algorithm>
#include <utility>
#include <iterator>
#include <memory>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <map>
#include <set>
#include <queue>
#include <stack>
#include <deque>
#include <bitset>
#include <list>
#include <functional>
#include <numeric>
#include <climits>
#include <cassert>
#include <ctime>
#include <cctype>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <complex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <assert.h>
#include "Node.hpp"
#include "ErrorCodes.h"
#include "Operations.h"
#include "BEpsilonTree.hpp"

using namespace std;

using ValueType = int;

bool testCondition(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "[PASS] " << message << "\n";
        return true;
    }
    else {
        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }
}

void testInsertWithInt(BEpsilonTree<int, int> tree, bool& allTestsPassed) {
    cout << "Testing Insert for B-Epsilon tree" << endl;
    allTestsPassed &= testCondition(tree.insert(10, 100) == ErrorCode::Success, "Insert key 10");
    allTestsPassed &= testCondition(tree.insert(20, 200) == ErrorCode::Success, "Insert key 20");
    allTestsPassed &= testCondition(tree.insert(5, 50) == ErrorCode::Success, "Insert key 5");
    allTestsPassed &= testCondition(tree.insert(6, 60) == ErrorCode::Success, "Insert key 6");
    allTestsPassed &= testCondition(tree.insert(12, 120) == ErrorCode::Success, "Insert key 12");
    allTestsPassed &= testCondition(tree.insert(1, 10) == ErrorCode::Success, "Insert key 1");
    allTestsPassed &= testCondition(tree.insert(2, 20) == ErrorCode::Success, "Insert key 2");
    allTestsPassed &= testCondition(tree.insert(3, 30) == ErrorCode::Success, "Insert key 3");
    allTestsPassed &= testCondition(tree.insert(11, 110) == ErrorCode::Success, "Insert key 11");
    allTestsPassed &= testCondition(tree.insert(4, 40) == ErrorCode::Success, "Insert key 4");
    allTestsPassed &= testCondition(tree.insert(21, 210) == ErrorCode::Success, "Insert key 21");
    allTestsPassed &= testCondition(tree.insert(50, 500) == ErrorCode::Success, "Insert key 50");

    tree.display(tree.root, 0);
}

void testSearch(BEpsilonTree<int, int> tree, bool& allTestsPassed) {
    cout << "Testing Search for B-Epsilon tree" << endl;
    allTestsPassed &= testCondition(tree.insert(10, 100) == ErrorCode::Success, "Insert key 10");
    allTestsPassed &= testCondition(tree.insert(20, 200) == ErrorCode::Success, "Insert key 20");
    allTestsPassed &= testCondition(tree.insert(5, 50) == ErrorCode::Success, "Insert key 5");
    allTestsPassed &= testCondition(tree.insert(6, 60) == ErrorCode::Success, "Insert key 6");
    allTestsPassed &= testCondition(tree.insert(12, 120) == ErrorCode::Success, "Insert key 12");
    allTestsPassed &= testCondition(tree.insert(1, 10) == ErrorCode::Success, "Insert key 1");
    allTestsPassed &= testCondition(tree.insert(2, 20) == ErrorCode::Success, "Insert key 2");
    allTestsPassed &= testCondition(tree.insert(3, 30) == ErrorCode::Success, "Insert key 3");
    allTestsPassed &= testCondition(tree.insert(11, 110) == ErrorCode::Success, "Insert key 11");
    allTestsPassed &= testCondition(tree.insert(4, 40) == ErrorCode::Success, "Insert key 4");
    allTestsPassed &= testCondition(tree.insert(21, 210) == ErrorCode::Success, "Insert key 21");

    tree.display(tree.root, 0);

    ValueType value;
    allTestsPassed &= testCondition(tree.search(10, value) == ErrorCode::Success && value == 100, "Validate key 10");
    allTestsPassed &= testCondition(tree.search(20, value) == ErrorCode::Success && value == 200, "Validate key 20");
    allTestsPassed &= testCondition(tree.search(5, value) == ErrorCode::Success && value == 50, "Validate key 5");
    allTestsPassed &= testCondition(tree.search(6, value) == ErrorCode::Success && value == 60, "Validate key 6");
    allTestsPassed &= testCondition(tree.search(12, value) == ErrorCode::Success && value == 120, "Validate key 12");
    allTestsPassed &= testCondition(tree.search(1, value) == ErrorCode::Success && value == 10, "Validate key 1");
    allTestsPassed &= testCondition(tree.search(2, value) == ErrorCode::Success && value == 20, "Validate key 2");
    allTestsPassed &= testCondition(tree.search(3, value) == ErrorCode::Success && value == 30, "Validate key 3");
    allTestsPassed &= testCondition(tree.search(11, value) == ErrorCode::Success && value == 110, "Validate key 11");
    allTestsPassed &= testCondition(tree.search(4, value) == ErrorCode::Success && value == 40, "Validate key 4");
    allTestsPassed &= testCondition(tree.search(21, value) == ErrorCode::Success && value == 210, "Validate key 21");
    //tree.search(100, value) == ErrorCode::KeyDoesNotExist, "search of an non existant key");

}

void testUpdate(BEpsilonTree<int, int> tree, bool& allTestsPassed) {
    cout << "Testing Update for B-Epsilon tree" << endl;
    allTestsPassed &= testCondition(tree.insert(1, 1) == ErrorCode::Success, "Insert key 10");
    allTestsPassed &= testCondition(tree.insert(2, 2) == ErrorCode::Success, "Insert key 20");
    allTestsPassed &= testCondition(tree.insert(3, 3) == ErrorCode::Success, "Insert key 5");
    allTestsPassed &= testCondition(tree.insert(0, 0) == ErrorCode::Success, "Insert key 1");
    allTestsPassed &= testCondition(tree.insert(4, 4) == ErrorCode::Success, "Insert key 6");
    allTestsPassed &= testCondition(tree.insert(5, 5) == ErrorCode::Success, "Insert key 12");
    cout << "Tree before updating" << endl;
    tree.display(tree.root, 0);

    allTestsPassed &= testCondition(tree.update(0, 1000) == ErrorCode::Success, "Update the value of key 0");
    allTestsPassed &= testCondition(tree.update(1, 10) == ErrorCode::Success, "Update the value of key 1");
    allTestsPassed &= testCondition(tree.update(2, 20) == ErrorCode::Success, "Update the value of key 2");
    allTestsPassed &= testCondition(tree.update(3, 30) == ErrorCode::Success, "Update the value of key 3");
    allTestsPassed &= testCondition(tree.update(4, 40) == ErrorCode::Success, "Update the value of key 4");
    allTestsPassed &= testCondition(tree.update(5, 50) == ErrorCode::Success, "Update the value of key 5");

    ValueType value;
    allTestsPassed &= testCondition(tree.search(0, value) == ErrorCode::Success && value == 1000, "Validate the value of key 0 after update");
    allTestsPassed &= testCondition(tree.search(1, value) == ErrorCode::Success && value == 10, "Validate the value of key 1 after update");
    allTestsPassed &= testCondition(tree.search(2, value) == ErrorCode::Success && value == 20, "Validate the value of key 2 after update");
    allTestsPassed &= testCondition(tree.search(3, value) == ErrorCode::Success && value == 30, "Validate the value of key 3 after update");
    allTestsPassed &= testCondition(tree.search(4, value) == ErrorCode::Success && value == 40, "Validate the value of key 4 after update");
    allTestsPassed &= testCondition(tree.search(5, value) == ErrorCode::Success && value == 50, "Validate the value of key 5 after update");

    cout << "Tree after updating" << endl;
    tree.display(tree.root, 0);

}

void testRemove(BEpsilonTree<int, int> tree, bool& allTestsPassed) {
    cout << "Testing Remove for B-Epsilon tree" << endl;
    // Step 1: Insert some keys
    allTestsPassed &= testCondition(tree.insert(10, 100) == ErrorCode::Success, "Insert key 10");
    allTestsPassed &= testCondition(tree.insert(20, 200) == ErrorCode::Success, "Insert key 20");
    allTestsPassed &= testCondition(tree.insert(5, 50) == ErrorCode::Success, "Insert key 5");
    allTestsPassed &= testCondition(tree.insert(6, 60) == ErrorCode::Success, "Insert key 6");
    allTestsPassed &= testCondition(tree.insert(12, 120) == ErrorCode::Success, "Insert key 12");
    allTestsPassed &= testCondition(tree.insert(1, 10) == ErrorCode::Success, "Insert key 1");
    allTestsPassed &= testCondition(tree.insert(2, 20) == ErrorCode::Success, "Insert key 2");
    allTestsPassed &= testCondition(tree.insert(3, 30) == ErrorCode::Success, "Insert key 3");
    allTestsPassed &= testCondition(tree.insert(30, 300) == ErrorCode::Success, "Insert key 30");

    std::cout << "Tree before removal:\n";
    tree.display(tree.root, 0);

    // Step 2: Remove a few keys
    allTestsPassed &= testCondition(tree.remove(1) == ErrorCode::Success, "Remove key 1");
    allTestsPassed &= testCondition(tree.remove(10) == ErrorCode::Success, "Remove key 10");
    allTestsPassed &= testCondition(tree.remove(20) == ErrorCode::Success, "Remove key 20");

    std::cout << "Tree after removing keys 1, 10, and 20:\n";
    tree.display(tree.root, 0);

    // Step 3: Validate removals
    ValueType value;
    allTestsPassed &= testCondition(tree.search(1, value) == ErrorCode::KeyDoesNotExist, "Validate key 1 is removed");
    allTestsPassed &= testCondition(tree.search(10, value) == ErrorCode::KeyDoesNotExist, "Validate key 10 is removed");
    allTestsPassed &= testCondition(tree.search(20, value) == ErrorCode::KeyDoesNotExist, "Validate key 20 is removed");

    // Step 4: Validate remaining keys
    allTestsPassed &= testCondition(tree.search(5, value) == ErrorCode::Success && value == 50, "Validate key 5 still exists");
    allTestsPassed &= testCondition(tree.search(6, value) == ErrorCode::Success && value == 60, "Validate key 6 still exists");
    allTestsPassed &= testCondition(tree.search(12, value) == ErrorCode::Success && value == 120, "Validate key 12 still exists");

    std::cout << "Tree after validation of removals:\n";
    tree.display(tree.root, 0);
}

void testRangeQuery(BEpsilonTree<int, int> tree, bool& allTestsPassed) {
    cout << "Testing Range Query for B-Epsilon tree" << endl;
    // Step 1: Insert some keys
    allTestsPassed &= testCondition(tree.insert(10, 100) == ErrorCode::Success, "Insert key 10");
    allTestsPassed &= testCondition(tree.insert(20, 200) == ErrorCode::Success, "Insert key 20");
    allTestsPassed &= testCondition(tree.insert(5, 50) == ErrorCode::Success, "Insert key 5");
    allTestsPassed &= testCondition(tree.insert(6, 60) == ErrorCode::Success, "Insert key 6");
    allTestsPassed &= testCondition(tree.insert(12, 120) == ErrorCode::Success, "Insert key 12");
    allTestsPassed &= testCondition(tree.insert(1, 10) == ErrorCode::Success, "Insert key 1");
    allTestsPassed &= testCondition(tree.insert(2, 20) == ErrorCode::Success, "Insert key 2");
    allTestsPassed &= testCondition(tree.insert(3, 30) == ErrorCode::Success, "Insert key 3");
    allTestsPassed &= testCondition(tree.insert(30, 300) == ErrorCode::Success, "Insert key 30");

    tree.display(tree.root, 0);

    auto result = tree.rangeQuery(1, 10);
    std::vector<std::pair<int, int>> expected = { {2, 20}, {3, 30}, {1, 10}, {5, 50}, {6, 60}, {10, 100} };
    allTestsPassed &= testCondition(result == expected, "Range query results [1, 10]");

    std::cout << "Range query results [1, 10]:\n";
    for (const auto& pair : result) {
        std::cout << "(" << pair.first << ", " << pair.second << ")\n";
    }
}

int main() {

    bool allTestsPassed = true;
    BEpsilonTree<int, int> tree1 = BEpsilonTree<int, int>(3, 3);
    testInsertWithInt(tree1, allTestsPassed);

    BEpsilonTree<int, int> tree2 = BEpsilonTree<int, int>(3, 3);
    testUpdate(tree2, allTestsPassed);

    BEpsilonTree<int, int> tree3 = BEpsilonTree<int, int>(3, 3);
    testSearch(tree3, allTestsPassed);

    BEpsilonTree<int, int> tree4 = BEpsilonTree<int, int>(3, 3);
    testRemove(tree4, allTestsPassed);

    BEpsilonTree<int, int> tree5 = BEpsilonTree<int, int>(3, 3);
    testRangeQuery(tree5, allTestsPassed);

    if (allTestsPassed) {
        std::cout << "All tests passed successfully.\n";
    }
    else {
        std::cerr << "Some tests failed. Please check the log above.\n";
    }

    return allTestsPassed ? 0 : 1;
}
