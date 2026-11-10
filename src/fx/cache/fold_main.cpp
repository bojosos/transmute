// fx-cache-fold.exe: folds translation-cache journals into large main files
// for guest processes, which start it detached at idle priority when they
// exit (cache_enable_fold_helper, owner: cache).
//   fx-cache-fold <cache directory> <stem file name>...

#include "fx/cache/cache.hpp"

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) { return transmute::fx::cache_fold_main(argc, argv); }
#else
#include "fx/host/text.hpp"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::wstring> wide;
    for (int i = 0; i < argc; ++i) wide.push_back(transmute::fx::host::from_utf8(argv[i]));
    std::vector<wchar_t*> pointers;
    for (auto& argument : wide) pointers.push_back(argument.data());
    pointers.push_back(nullptr);
    return transmute::fx::cache_fold_main(argc, pointers.data());
}
#endif
