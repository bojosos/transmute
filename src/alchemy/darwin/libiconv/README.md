# libiconv

`/usr/lib/libiconv.2.dylib` (both builds), the GNU interface macOS ships.

* **Real:** `iconv_open`, `iconv` and `iconv_close` (and their `lib`-prefixed
  names) between UTF-8, UTF-16 (BE, LE, with a byte-order mark), UCS-2,
  UTF-32 / UCS-4, ASCII and Latin-1, with `//TRANSLIT` and `//IGNORE`, and
  Darwin's errno values (E2BIG, EILSEQ, EINVAL, EBADF).
* **Unavailable, as documented:** other character sets: `iconv_open` fails
  with EINVAL.
* **Host dependencies:** none (pure computation).
* **Registration:** `darwin_headless::install_libiconv`, in the headless
  framework set (`darwin/frameworks.cpp`, every build).
* **Tests:** `tests/libiconv_tests.cpp` (ctest `alchemy_gui_libiconv`) and the
  headless `alchemy_frameworks`.
