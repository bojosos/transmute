// fx-cache-fold.exe: folds translation-cache journals into large main files
// for guest processes, which start it detached at idle priority when they
// exit (cache_enable_fold_helper, owner: cache).
//   fx-cache-fold <cache directory> <stem file name>...

#include "fx/cache/cache.hpp"

int wmain(int argc, wchar_t** argv) { return transmute::fx::cache_fold_main(argc, argv); }
