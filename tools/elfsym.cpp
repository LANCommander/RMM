// rmm_elfsym - check gamedata symbols against a server binary offline.
//
//   rmm_elfsym <elf binary> <gamedata.ini>      verify every _Z... name in [linux]
//   rmm_elfsym <elf binary> <mangled name>...   print addresses
//
// Exit code 0 only if every symbol resolved. Uses the same parser as the
// in-server code (src/games/common/elf_symbols.cpp).
#include "games/common/elf_symbols.h"
#include "core/util.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: rmm_elfsym <elf> <gamedata.ini | symbol...>\n");

        return 2;
    }

    rmm::elf::SymbolTable table;
    std::string err;

    if (!table.Load(argv[1], 0, err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());

        return 1;
    }

    std::printf("%zu symbols indexed\n", table.Size());

    std::vector<std::pair<std::string, std::string>> wanted;  // label, mangled
    std::string second = argv[2];

    if (second.size() > 4 && second.substr(second.size() - 4) == ".ini") {
        rmm::Ini ini;

        if (!ini.Load(second)) {
            std::fprintf(stderr, "cannot read %s\n", second.c_str());

            return 1;
        }

        // Every [linux] value is a symbol name except flags like "enabled = 1"
        // and lists like "gpm_names = ,GPM_CTF,..." (symbols never contain commas).
        for (auto& kv : ini.Section("linux")) {
            double num;

            if (!kv.second.empty() && !rmm::ParseDouble(kv.second, num) && kv.second.find(',') == std::string::npos)
                wanted.emplace_back(kv.first, kv.second);
        }
    } else {
        for (int i = 2; i < argc; ++i)
            wanted.emplace_back(argv[i], argv[i]);
    }

    int missing = 0;

    for (auto& w : wanted) {
        uintptr_t a = table.Find(w.second);

        if (!a)
            ++missing;

        std::printf("%-18s %08lx  %s\n", w.first.c_str(), static_cast<unsigned long>(a), a ? "" : "MISSING");
    }

    return missing ? 1 : 0;
}
