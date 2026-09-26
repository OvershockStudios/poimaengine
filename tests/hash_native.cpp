// SPDX-License-Identifier: Apache-2.0
#include "poima/assets.hpp"
#include <iostream>
int main() {
    const std::pair<std::string,std::string> vectors[]={
        {"","e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc","ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq","248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {std::string(1000000,'a'),"cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"}};
    for(const auto& [text,expected]:vectors)if(poima::sha256(std::as_bytes(std::span(text.data(),text.size())))!=expected) { std::cerr<<"SHA-256 known-answer failure\n";return 1; }
    std::cout<<"SHA-256 empty, short, multi-block and million-byte known answers passed.\n";
}
