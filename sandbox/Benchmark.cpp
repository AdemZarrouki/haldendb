#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>
#include "BEpsilonTree.hpp"
#include <numeric>
#include <cassert>
#include <fstream>
#include <array>

using namespace std;


void benchmarkBEpsilonTreeint_32(size_t size, size_t nMaxNumber, ofstream& outFile) {
    std::vector<int32_t> random_numbers1(nMaxNumber);
    std::iota(random_numbers1.begin(), random_numbers1.end(), 1);
    std::random_device rd1; // Obtain a random number from hardware
    std::mt19937 eng1(rd1()); // Seed the generator
    std::shuffle(random_numbers1.begin(), random_numbers1.end(), eng1);

    BEpsilonTree<int32_t, int32_t> tree(size, size / 2);

    // *** Warming Up the Tree (Insert 50M Elements) ***
    std::cout << "[Warming Up] Inserting " << nMaxNumber << " elements...\n";
    for (size_t i = 0; i < nMaxNumber; i++) {
        ErrorCode code = tree.insert(random_numbers1[i], random_numbers1[i]);
        assert(code == ErrorCode::Success);
    }
    std::cout << "[Warm-Up Complete] Tree Ready.\n";
    outFile << "[Warm-Up Complete] Tree Ready.\n";
    
    // *** Benchmark: Search (Find) 50M Elements ***
    std::chrono::steady_clock::time_point beginFind = std::chrono::steady_clock::now();
    //auto beginFind = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int32_t value;
        ErrorCode code = tree.search(random_numbers1[i], value);
        assert(value == random_numbers1[i]);
    }
    auto endFind = std::chrono::steady_clock::now();
    auto findTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endFind - beginFind).count();
    auto findTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endFind - beginFind).count();
    std::cout << ">> [Benchmark] 50M Finds completed in [Time: " << findTimeMicroseconds << "us, " << findTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Finds completed in [Time: " << findTimeMicroseconds << "us, " << findTimeNanoseconds << "ns]\n";


    // *** Benchmark: Insert 50M New Elements ***
    std::chrono::steady_clock::time_point beginInsert = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int32_t newKey = random_numbers1[i] + nMaxNumber;
        ErrorCode code = tree.insert(newKey, newKey);
        assert(code == ErrorCode::Success);
    }
    auto endInsert = std::chrono::steady_clock::now();
    auto insertTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endInsert - beginInsert).count();
    auto insertTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endInsert - beginInsert).count();
    std::cout << ">> [Benchmark] 50M Inserts completed in [Time: " << insertTimeMicroseconds << "us, " << insertTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Inserts completed in [Time: " << insertTimeMicroseconds << "us, " << insertTimeNanoseconds << "ns]\n";


    // *** Benchmark: Update 50M Existing Elements ***
    std::chrono::steady_clock::time_point beginUpdate = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int32_t newValue = random_numbers1[i] * 100;
        ErrorCode code = tree.update(random_numbers1[i], newValue);
        assert(code == ErrorCode::Success);
    }
    auto endUpdate = std::chrono::steady_clock::now();
    auto updateTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endUpdate - beginUpdate).count();
    auto updateTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endUpdate - beginUpdate).count();
    std::cout << ">> [Benchmark] 50M Updates completed in [Time: " << updateTimeMicroseconds << "us, " << updateTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Updates completed in [Time: " << updateTimeMicroseconds << "us, " << updateTimeNanoseconds << "ns]\n";

   
    // *** Benchmark: Delete 50M Elements ***
    std::chrono::steady_clock::time_point beginDelete = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        ErrorCode code = tree.remove(random_numbers1[i]);
        assert(code == ErrorCode::Success);
    }
    auto endDelete = std::chrono::steady_clock::now();
    auto deleteTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endDelete - beginDelete).count();
    auto deleteTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endDelete - beginDelete).count();
    std::cout << ">> [Benchmark] 50M Deletes completed in [Time: " << deleteTimeMicroseconds << "us, " << deleteTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Deletes completed in [Time: " << deleteTimeMicroseconds << "us, " << deleteTimeNanoseconds << "ns]\n";

    std::cout << "[Benchmark Complete] Results saved.\n";
    outFile << "[Benchmark Complete] Results saved.\n";

}

void benchmarkBEpsilonTreeint_64(size_t size, size_t nMaxNumber, ofstream& outFile) {
    std::vector<int64_t> random_numbers1(nMaxNumber);
    std::iota(random_numbers1.begin(), random_numbers1.end(), 1);
    std::random_device rd1; // Obtain a random number from hardware
    std::mt19937 eng1(rd1()); // Seed the generator
    std::shuffle(random_numbers1.begin(), random_numbers1.end(), eng1);

    BEpsilonTree<int64_t, int64_t> tree(size, size / 2);

    // *** Warming Up the Tree (Insert 50M Elements) ***
    std::cout << "[Warming Up] Inserting " << nMaxNumber << " elements...\n";
    for (size_t i = 0; i < nMaxNumber; i++) {
        ErrorCode code = tree.insert(random_numbers1[i], random_numbers1[i]);
        assert(code == ErrorCode::Success);
    }
    std::cout << "[Warm-Up Complete] Tree Ready.\n";
    outFile << "[Warm-Up Complete] Tree Ready.\n";


    // *** Benchmark: Search (Find) 50M Elements ***
    std::chrono::steady_clock::time_point beginFind = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int64_t value;
        ErrorCode code = tree.search(random_numbers1[i], value);
        assert(value == random_numbers1[i]);
    }
    auto endFind = std::chrono::steady_clock::now();
    auto findTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endFind - beginFind).count();
    auto findTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endFind - beginFind).count();
    std::cout << ">> [Benchmark] 50M Finds completed in [Time: " << findTimeMicroseconds << "us, " << findTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Finds completed in [Time: " << findTimeMicroseconds << "us, " << findTimeNanoseconds << "ns]\n";


    // *** Benchmark: Insert 50M New Elements ***
    std::chrono::steady_clock::time_point beginInsert = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int64_t newKey = random_numbers1[i] + nMaxNumber;
        ErrorCode code = tree.insert(newKey, newKey);
        assert(code == ErrorCode::Success);
    }
    auto endInsert = std::chrono::steady_clock::now();
    auto insertTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endInsert - beginInsert).count();
    auto insertTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endInsert - beginInsert).count();
    std::cout << ">> [Benchmark] 50M Inserts completed in [Time: " << insertTimeMicroseconds << "us, " << insertTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Inserts completed in [Time: " << insertTimeMicroseconds << "us, " << insertTimeNanoseconds << "ns]\n";




    // *** Benchmark: Update 50M Existing Elements ***
    std::chrono::steady_clock::time_point beginUpdate = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int64_t newValue = random_numbers1[i] * 100;
        ErrorCode code = tree.update(random_numbers1[i], newValue);
        assert(code == ErrorCode::Success);
    }
    auto endUpdate = std::chrono::steady_clock::now();
    auto updateTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endUpdate - beginUpdate).count();
    auto updateTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endUpdate - beginUpdate).count();
    std::cout << ">> [Benchmark] 50M Updates completed in [Time: " << updateTimeMicroseconds << "us, " << updateTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Updates completed in [Time: " << updateTimeMicroseconds << "us, " << updateTimeNanoseconds << "ns]\n";


    // *** Benchmark: Delete 50M Elements ***
    std::chrono::steady_clock::time_point beginDelete = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        ErrorCode code = tree.remove(random_numbers1[i]);
        assert(code == ErrorCode::Success);
    }

    auto endDelete = std::chrono::steady_clock::now();
    auto deleteTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endDelete - beginDelete).count();
    auto deleteTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endDelete - beginDelete).count();
    std::cout << ">> [Benchmark] 50M Deletes completed in [Time: " << deleteTimeMicroseconds << "us, " << deleteTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Deletes completed in [Time: " << deleteTimeMicroseconds << "us, " << deleteTimeNanoseconds << "ns]\n";

    std::cout << "[Benchmark Complete] Results saved.\n";
    outFile << "[Benchmark Complete] Results saved.\n";
}


std::vector<std::array<unsigned char, 16>> generateRandomStrings(size_t count) {
    std::vector<std::array<unsigned char, 16>> randomStrings(count); // Preallocate vector

    std::random_device rd;
    std::mt19937 gen(rd());  // Mersenne Twister RNG
    std::uniform_int_distribution<int> dist(0, 255);  // Printable ASCII characters

    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < 16; ++j) {
            randomStrings[i][j] = static_cast<unsigned char>(dist(gen));  // Assign random character
        }
    }

    return randomStrings;
}


void benchmarkingString(size_t size, size_t nMaxNumber, ofstream& outFile) {
    std::vector<std::array<unsigned char, 16>> random_strings = generateRandomStrings(nMaxNumber);

    std::vector<int64_t> random_values(nMaxNumber);
    std::iota(random_values.begin(), random_values.end(), 1);
    std::random_device rd1; // Obtain a random number from hardware
    std::mt19937 eng1(rd1()); // Seed the generator
    std::shuffle(random_values.begin(), random_values.end(), eng1);


    BEpsilonTree< std::array<unsigned char, 16>, int64_t> tree(size, size / 2);

    // *** Warming Up the Tree (Insert 50M Elements) ***
    std::cout << "[Warming Up] Inserting " << nMaxNumber << " elements...\n";
    for (size_t i = 0; i < nMaxNumber; i++) {
        ErrorCode code = tree.insert(random_strings[i], random_values[i]);
        assert(code == ErrorCode::Success);
    }
    std::cout << "[Warm-Up Complete] Tree Ready.\n";
    outFile << "[Warm-Up Complete] Tree Ready.\n";
    
    // *** Benchmark: Search (Find) 50M Elements ***
    std::chrono::steady_clock::time_point beginFind = std::chrono::steady_clock::now();
    //auto beginFind = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int64_t value;
        ErrorCode code = tree.search(random_strings[i], value);
        assert(value == random_values[i]);
    }
    auto endFind = std::chrono::steady_clock::now();
    auto findTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endFind - beginFind).count();
    auto findTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endFind - beginFind).count();
    std::cout << ">> [Benchmark] 50M Finds completed in [Time: " << findTimeMicroseconds << "us, " << findTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Finds completed in [Time: " << findTimeMicroseconds << "us, " << findTimeNanoseconds << "ns]\n";


    // *** Benchmark: Insert 50M New Elements ***
    std::vector<std::array<unsigned char, 16>> random_strings_new = generateRandomStrings(nMaxNumber);
    std::chrono::steady_clock::time_point beginInsert = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
		int64_t new_value = random_values[i] * 10;
        ErrorCode code = tree.insert(random_strings_new[i], new_value);
        assert(code == ErrorCode::Success);
    }
    auto endInsert = std::chrono::steady_clock::now();
    auto insertTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endInsert - beginInsert).count();
    auto insertTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endInsert - beginInsert).count();
    std::cout << ">> [Benchmark] 50M Inserts completed in [Time: " << insertTimeMicroseconds << "us, " << insertTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Inserts completed in [Time: " << insertTimeMicroseconds << "us, " << insertTimeNanoseconds << "ns]\n";


    // *** Benchmark: Update 50M Existing Elements ***
    std::chrono::steady_clock::time_point beginUpdate = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        int64_t new_value = random_values[i] * 100;
        ErrorCode code = tree.update(random_strings[i], new_value);
        assert(code == ErrorCode::Success);
    }
    auto endUpdate = std::chrono::steady_clock::now();
    auto updateTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endUpdate - beginUpdate).count();
    auto updateTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endUpdate - beginUpdate).count();
    std::cout << ">> [Benchmark] 50M Updates completed in [Time: " << updateTimeMicroseconds << "us, " << updateTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Updates completed in [Time: " << updateTimeMicroseconds << "us, " << updateTimeNanoseconds << "ns]\n";


    // *** Benchmark: Delete 50M Elements ***
    std::chrono::steady_clock::time_point beginDelete = std::chrono::steady_clock::now();
    for (size_t i = 0; i < nMaxNumber; i++) {
        ErrorCode code = tree.remove(random_strings_new[i]);
        assert(code == ErrorCode::Success);
    }
    auto endDelete = std::chrono::steady_clock::now();
    auto deleteTimeMicroseconds = chrono::duration_cast<chrono::microseconds>(endDelete - beginDelete).count();
    auto deleteTimeNanoseconds = chrono::duration_cast<chrono::nanoseconds>(endDelete - beginDelete).count();
    std::cout << ">> [Benchmark] 50M Deletes completed in [Time: " << deleteTimeMicroseconds << "us, " << deleteTimeNanoseconds << "ns]\n";
    outFile << ">> [Benchmark] 50M Deletes completed in [Time: " << deleteTimeMicroseconds << "us, " << deleteTimeNanoseconds << "ns]\n"; 

    std::cout << "[Benchmark Complete] Results saved.\n";
    outFile << "[Benchmark Complete] Results saved.\n";
}


int main() {
    //string filePath = "C:/Users/zarroa/Desktop/benchmark_results.txt";
    string filePath = "C:/Users/zarroa/Desktop/benchmark_results.txt";
    ofstream outFile(filePath);
    if (!outFile) {
        cerr << "Error: Could not open file at " << filePath << " for writing!\n";
        return 1;
    }

	size_t nMaxNumber = 50000000;
    for (size_t nDegree = 500; nDegree < 1850; nDegree = nDegree + 50)
    {
        std::cout << "||||||| Running 'test_for_ints32' for nDegree:" << nDegree << std::endl;
        outFile << "||||||| Running 'test_for_ints32' for nDegree: " << nDegree << endl;

        //std::cout << "||||||| Running 'test_for_strings' for nDegree:" << nDegree << std::endl;
        //outFile << "||||||| Running 'test_for_strings' for nDegree: " << nDegree << endl;


        benchmarkBEpsilonTreeint_32(nDegree, nMaxNumber, outFile);
    }

    outFile.close();
    std::cout << "Benchmark results saved to " << filePath << endl;
    return 0;

}
