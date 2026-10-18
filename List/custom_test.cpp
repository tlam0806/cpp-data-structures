#include <iostream>
#include "List.hpp"

using lam::List;

struct A {
    ~A() {
        std::cout << "destroying\n";
    }
};

int main() {
    A tmp1, tmp2;
    List<A> list;
    list.push_back(tmp1);
    list.push_back(tmp2);
}