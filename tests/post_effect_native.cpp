// SPDX-License-Identifier: Apache-2.0
#include "poima/post_effect.hpp"
#include "poima/assets.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>

namespace {
using Json = nlohmann::json;
using Words = std::vector<std::uint32_t>;
constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void check(bool condition, std::string_view reason) {
    if (!condition) throw std::runtime_error(std::string(reason));
}

std::string sha(std::string_view data) {
    return poima::sha256(std::as_bytes(std::span(data)));
}

void reject(std::string_view changed, std::string_view expected_sha, std::string_view reason) {
    try {
        (void)poima::parse_post_effect(changed, expected_sha);
    } catch (const std::runtime_error& error) {
        if (std::string_view(error.what()).find(reason) == std::string_view::npos)
            throw std::runtime_error("Expected '" + std::string(reason) + "', got '" + error.what() + "'.");
        return;
    }
    throw std::runtime_error("Post-effect artifact unexpectedly accepted. Expected '" + std::string(reason) + "'.");
}

void reject(const Json& changed, std::string_view reason) {
    const auto bytes = changed.dump();
    reject(bytes, sha(bytes), reason);
}

std::string pack(const Words& words) {
    std::string bytes;
    for (auto value : words)
        for (unsigned byte = 0; byte < 4; ++byte, value >>= 8)
            bytes.push_back(static_cast<char>(value & 255));
    return bytes;
}

std::string base64(std::string_view bytes) {
    std::string encoded;
    for (std::size_t offset = 0; offset < bytes.size(); offset += 3) {
        const auto count = std::min(bytes.size() - offset, std::size_t(3));
        unsigned value = 0;
        for (std::size_t n = 0; n < count; ++n)
            value |= static_cast<unsigned>(static_cast<unsigned char>(bytes[offset + n])) << (16 - 8 * n);
        for (unsigned n = 0; n < 4; ++n)
            encoded.push_back(n <= count ? alphabet[(value >> (18 - 6 * n)) & 63] : '=');
    }
    return encoded;
}

std::size_t instruction(const Words& words, unsigned opcode, auto matches) {
    for (std::size_t offset = 5; offset < words.size(); offset += words[offset] >> 16)
        if ((words[offset] & 65535) == opcode && matches(offset)) return offset;
    throw std::runtime_error("Pinned shader fixture instruction missing.");
}

void reject_shader(Json changed, const Words& words, std::string_view reason) {
    const auto raw = pack(words);
    changed["spirv"]["data"] = base64(raw);
    changed["spirv"]["sha256"] = sha(raw);
    reject(changed, reason);
}
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "Usage: post-effect-native <compiled sample artifact>.");
        std::ifstream input(argv[1], std::ios::binary);
        std::string bytes(poima::max_post_effect_bytes + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        const auto fixture_hash = sha(bytes);
        const auto effect = poima::parse_post_effect(bytes, fixture_hash);
        check(effect.sha256 == fixture_hash && effect.id == "soft_vignette" && effect.words.size() > 5,
            "Unexpected admitted effect identity.");
        const auto fixture = Json::parse(bytes);
        check(sha(pack(effect.words)) == fixture.at("spirv").at("sha256").get<std::string>(),
            "The admitted words differ from the pinned shader bytes.");

        reject(bytes, std::string(64, '0'), "artifact SHA-256 differs");
        reject(bytes, "", "Expected a lowercase");
        reject(std::string(poima::max_post_effect_bytes + 1, ' '), fixture_hash, "byte budget");
        const std::string duplicate = "{\"version\":1," + bytes.substr(1);
        reject(duplicate, sha(duplicate), "Duplicate post-effect JSON key");

        auto changed = fixture;
        changed["manifest"]["slot"] = "unrelated-render-stage";
        reject(changed, "HDR slot");
        changed = fixture;
        changed["manifest"]["inputs"].push_back("scene_depth");
        reject(changed, "HDR slot");
        changed = fixture;
        changed["compiler"]["files"]["bin/dxc"] = std::string(64, '1');
        reject(changed, "pinned producer");
        changed = fixture;
        changed["sources"][0]["path"] = "../Feather.hlsli";
        reject(changed, "portable post-effect source path");
        changed = fixture;
        changed["sources"][0]["sha256"] = std::string(64, 'a');
        reject(changed, "source closure hash differs");
        changed = fixture;
        std::swap(changed["sources"][0], changed["sources"][1]);
        reject(changed, "sorted and unique");
        changed = fixture;
        changed["interface"]["binding"] = 1;
        reject(changed, "interface metadata");
        changed = fixture;
        check(changed["spirv"]["data"].get_ref<std::string&>().ends_with('='), "Expected padded fixture base64.");
        auto& code = changed["spirv"]["data"].get_ref<std::string&>();
        const auto last_symbol = code.find_last_not_of('=');
        code[last_symbol] = alphabet[alphabet.find(code[last_symbol]) | 1];
        reject(changed, "base64 padding bits");
        changed = fixture;
        changed["spirv"]["sha256"] = std::string(64, 'b');
        reject(changed, "SPIR-V SHA-256 differs");
        changed = fixture;
        changed["spirv"]["instructions"] = changed["spirv"]["instructions"].get<unsigned>() + 1;
        reject(changed, "instruction count differs");

        const auto fetch = instruction(effect.words, 95, [](std::size_t) { return true; });
        auto wrong_words = effect.words;
        wrong_words[fetch] = 95;
        reject_shader(fixture, wrong_words, "Malformed post-effect SPIR-V instruction");
        wrong_words = effect.words;
        wrong_words[fetch] = (wrong_words[fetch] & 0xffff0000u) | 99u;
        reject_shader(fixture, wrong_words, "outside the post-effect profile");
        wrong_words = effect.words;
        const auto binding = instruction(wrong_words, 71, [&](std::size_t offset) { return wrong_words[offset + 2] == 33; });
        wrong_words[binding + 3] = 1;
        reject_shader(fixture, wrong_words, "descriptor set, binding or fragment interface differs");
        wrong_words = effect.words;
        const auto entry = instruction(wrong_words, 15, [](std::size_t) { return true; });
        wrong_words[entry + 1] = 5;
        reject_shader(fixture, wrong_words, "fixed post-effect fragment entry");
        wrong_words = effect.words;
        const auto coordinate_id = wrong_words[fetch + 4];
        const auto coordinate = instruction(wrong_words, 79, [&](std::size_t offset) {
            return wrong_words[offset + 2] == coordinate_id;
        });
        check(wrong_words[coordinate] >> 16 == 7, "Expected two selected components in the fixture coordinate.");
        std::swap(wrong_words[coordinate + 5], wrong_words[coordinate + 6]);
        reject_shader(fixture, wrong_words, "fetch the current pixel");
        wrong_words = effect.words;
        const auto mip_id = wrong_words[fetch + 6];
        const auto mip = instruction(wrong_words, 43, [&](std::size_t offset) { return wrong_words[offset + 2] == mip_id; });
        wrong_words[mip + 3] = 1;
        reject_shader(fixture, wrong_words, "mip zero");
        std::cout << "Post-effect artifact identity, source inventory, base64, and native fragment profile checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
