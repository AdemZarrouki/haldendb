#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>
#include "BEpsilonTree.hpp"
#include <numeric>

using namespace std;

// Function to generate a list of unique random numbers
vector<int> generateRandomNumbers(size_t size) {
    vector<int> numbers(size);
    iota(numbers.begin(), numbers.end(), 1);
    random_device rd;
    mt19937 g(rd());
    shuffle(numbers.begin(), numbers.end(), g);
    return numbers;
}

// Function to measure execution time of a task
template <typename Func>
double measureExecutionTime(Func&& func) {
    auto start = chrono::high_resolution_clock::now();
    func();
    auto end = chrono::high_resolution_clock::now();
    return chrono::duration<double, milli>(end - start).count();
}

// Perform operations on the tree and record times
void benchmarkBEpsilonTree(size_t size) {
    cout << "Benchmarking with " << size << " numbers...\n";

    BEpsilonTree<int, int> tree(10000, 10000);

    vector<int> numbers = generateRandomNumbers(size);

    double insertTime = measureExecutionTime([&]() {
        for (const auto& num : numbers) {
            tree.insert(num, num * 10);
        }
        });

    cout << "Insertion Time: " << insertTime / 1000.0 << " seconds\n";

    double searchTime = measureExecutionTime([&]() {
        for (const auto& num : numbers) {
            int value;
            tree.search(num, value);
        }
        });

    cout << "Search Time: " << searchTime / 1000.0 << " seconds\n";

    double deleteTime = measureExecutionTime([&]() {
        for (const auto& num : numbers) {
            tree.remove(num);
        }
        });

    cout << "Deletion Time: " << deleteTime / 1000.0 << " seconds\n";

    cout << "Benchmark for " << size << " numbers completed.\n\n";
}

int main() {
    vector<size_t> testSizes = { 100, 1000000, 10000000, 100000000 };

    for (const auto& size : testSizes) {
        benchmarkBEpsilonTree(size);
    }

    return 0;
}
