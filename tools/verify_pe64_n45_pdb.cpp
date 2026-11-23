#include "xfrontend/pe64_pdb.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

bool read_file(const std::string& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

std::string guid_text(const std::array<std::uint8_t, 16U>& guid) {
    static const char digits[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(32U);
    for (const std::uint8_t byte : guid) {
        result.push_back(digits[(byte >> 4U) & 0x0fU]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: verify_pe64_n45_pdb <input.dll> <input.pdb>\n";
        return 2;
    }
    std::vector<std::uint8_t> pe;
    std::vector<std::uint8_t> pdb;
    if (!read_file(argv[1], pe) || !read_file(argv[2], pdb)) {
        std::cerr << "cannot read PE/PDB input pair\n";
        return 3;
    }
    const auto pair = xfrontend::verify_pe64_pdb_pair(pe, pdb);
    if (!pair.valid) {
        std::cerr << "PE/PDB identity verification failed: " << pair.error << '\n';
        if (!pair.codeview.error.empty()) std::cerr << "CodeView: " << pair.codeview.error << '\n';
        if (!pair.pdb.error.empty()) std::cerr << "PDB: " << pair.pdb.error << '\n';
        return 4;
    }
    std::cout << "PDB MSF 7.0 page size: " << pair.pdb.page_size << '\n';
    std::cout << "PDB streams: " << pair.pdb.stream_count << '\n';
    std::cout << "PDB GUID: " << guid_text(pair.pdb.guid) << '\n';
    std::cout << "PDB age: " << pair.pdb.age << '\n';
    std::cout << "PE RSDS path: " << pair.codeview.pdb_path << '\n';
    std::cout << "PE/PDB GUID+Age match: YES\n";
    return 0;
}
