#include <cstddef>
#include <iostream>
#include <iterator>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const unsigned char*, std::size_t);

int main() {
    const std::string input{
        std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>()};
    return LLVMFuzzerTestOneInput(
        reinterpret_cast<const unsigned char*>(input.data()), input.size());
}
