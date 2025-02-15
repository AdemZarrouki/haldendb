/*#include <iostream>
#include <vector>
#include <algorithm>
#include <chrono>
#include <random>
#include "BEpsilonTree.hpp"
#include <numeric>
#include <cassert>

using namespace std;

void int_test1(BEpsilonTree<int, int> tree, size_t nMaxNumber) {

    std::vector<int> random_numbers(nMaxNumber);//50000000);
    std::iota(random_numbers.begin(), random_numbers.end(), 1); // Fill vector with 1 to 5,000,000

    std::random_device rd; // Obtain a random number from hardware
    std::mt19937 eng(rd()); // Seed the generator
    std::shuffle(random_numbers.begin(), random_numbers.end(), eng);

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    for (size_t nTestCntr = 0; nTestCntr < 2; nTestCntr++)
    {
        for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr = nCntr + 1)
        {
            ErrorCode code = tree.insert(random_numbers[nCntr], random_numbers[nCntr]);
            assert(code == ErrorCode::Success);
        }

        for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode code = tree.search(random_numbers[nCntr], nValue);

            assert(nValue == random_numbers[nCntr]);
        }

        for (size_t nCntr = 0; nCntr < nMaxNumber; nCntr = nCntr + 2)
        {
            ErrorCode code = tree.remove(random_numbers[nCntr]);

            assert(code == ErrorCode::Success);
        }
        for (size_t nCntr = 1; nCntr < nMaxNumber; nCntr = nCntr + 2)
        {
            ErrorCode code = tree.remove(random_numbers[nCntr]);

            assert(code == ErrorCode::Success);
        }

        for (int nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode code = tree.search(random_numbers[nCntr], nValue);

            assert(code == ErrorCode::KeyDoesNotExist);
        }
    }

    for (size_t nTestCntr = 0; nTestCntr < 2; nTestCntr++)
    {
        for (int nCntr = nMaxNumber; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = tree.insert(nCntr, nCntr);
            assert(ec == ErrorCode::Success);

        }
        for (int nCntr = nMaxNumber - 1; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = tree.insert(nCntr, nCntr);
            assert(ec == ErrorCode::Success);
        }

        for (int nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode ec = tree.search(nCntr, nValue);

            assert(nValue == nCntr && ec == ErrorCode::Success);
        }

        for (int nCntr = nMaxNumber; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = tree.remove(nCntr);
            assert(ec == ErrorCode::Success);
        }

        for (int nCntr = nMaxNumber - 1; nCntr >= 0; nCntr = nCntr - 2)
        {
            ErrorCode ec = tree.remove(nCntr);
            assert(ec == ErrorCode::Success);
        }

        for (int nCntr = 0; nCntr < nMaxNumber; nCntr++)
        {
            int nValue = 0;
            ErrorCode ec = tree.search(nCntr, nValue);

            assert(ec == ErrorCode::KeyDoesNotExist);

        }
    }

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::cout
        << ">> int_test [Time: "
        << std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count() << "us"
        << ", " << std::chrono::duration_cast<std::chrono::nanoseconds> (end - begin).count() << "ns]"
        << std::endl;
}

void test_for_ints1()
{
    for (size_t nDegree = 1000; nDegree < 2000; nDegree = nDegree + 200)
    {
        std::cout << "||||||| Running 'test_for_ints' for nDegree:" << nDegree << std::endl;
        BEpsilonTree<int, int> tree(nDegree, nDegree / 2);
        int_test1(tree, 5);
        std::cout << std::endl;
    }
}

void quick_test1()
{
    for (size_t idx = 0; idx < 5; idx++) {
        test_for_ints1();
    }
}

int main(int argc, char* argv[])
{
    //fptree_bm();
    quick_test1();
    return 0;
}

*/