// The randomizer's generator from the command line, to make and check seeds without the launcher (docs/randomizer.md).
//   rando_gen DAT GAME_DIR OUT_DIR SEED [options]     one run into OUT_DIR
//   rando_gen DAT GAME_DIR OUT_DIR batch N [options]  N seeds ("1".."N") into OUT_DIR in turn: how many fail
// options: fs Fluorite-sanity, lc long challenges, dw door warp, kc keep consumables
#include "../../launcher/randomizer.h"
#include <cstdlib>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: rando_gen DAT GAME_DIR OUT_DIR SEED|batch N [fs] [lc] [dw] [kc]\n"); return 2; }
    std::vector<uint8_t> dat;
    if (!rando::read_file(argv[1], dat)) { fprintf(stderr, "can't read %ls\n", argv[1]); return 1; }
    std::string text(dat.begin(), dat.end());
    rando::Options o;
    int batch = !wcscmp(argv[4], L"batch") && argc > 5 ? _wtoi(argv[5]) : 0;
    for (int i = batch ? 6 : 5; i < argc; i++) { if (!wcscmp(argv[i], L"fs")) o.fluorite_sanity = true; if (!wcscmp(argv[i], L"lc")) o.long_challenges = true;
        if (!wcscmp(argv[i], L"dw")) o.doors = true; if (!wcscmp(argv[i], L"kc")) o.keep_consumables = true; }
    int fails = 0;
    for (int n = 1; n <= (batch ? batch : 1); n++) {
        // UTF-8, as the launcher passes the page's text: a non-ASCII seed narrowed byte by byte made another run
        char seed[256] = "";
        if (!batch) WideCharToMultiByte(CP_UTF8, 0, argv[4], -1, seed, sizeof seed, 0, 0);
        o.seed = batch ? std::to_string(n) : std::string(seed);
        std::string err = rando::generate(text, argv[2], argv[3], o);
        if (!err.empty()) { fails++; printf("seed %s: %s\n", o.seed.c_str(), err.c_str()); }
    }
    printf("%d of %d seeds made\n", (batch ? batch : 1) - fails, batch ? batch : 1);
    return fails ? 1 : 0;
}
