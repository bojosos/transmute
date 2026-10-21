# Provider kits

These are the helper kits that several frameworks share. They are named by
what they do; comments say which program first needed them.

| Kit | Namespace | Used by |
| --- | --- | --- |
| `completion/` | `completion_kit` (over `support_kit::Support`) and the data, runtime and graphics helpers; its groups complete the classes other providers declare | Foundation, CoreFoundation, CoreServices, libSystem, dispatch, compression, identities, AppKit, QuartzCore, media |
| `support/` | `support_kit::Support` (class, method and fill-only registration) | QuartzCore, AppKit, CoreGraphics, Foundation, runloop, identities |
| `export_fill/` | `export_fill::Provider` (fill-only over a ProcessContext) | libSystem, dispatch, compression, identities, CoreFoundation, Foundation, CoreServices, Security, IOKit |
| `fwgaps/` | the fill-only kit | CoreFoundation, Foundation, AppKit, CoreText, identities |

Each kit also keeps its **install group**. An install group is an ordered
list of calls into the frameworks' entry points (`install_<framework>_<area>`),
which the installer table (`src/alchemy/gui/install.cpp`) runs as one row,
because the calls share the kit's state object.

* Every name still has one owner: the audit is at 0 duplicates.
* The group's order is the order of the old app installer, so behaviour is
  unchanged.

`tests/` subdirectories here hold the cross-framework tests: cases that need
several frameworks together.

The former `chrome/` kit is the value-class provider, `gui::values::Provider`
(`src/alchemy/gui/value_provider.hpp`), next to `gui::fill::Provider`.

**Later:** merge the kits with `gui::fill::Provider`, `gui::values::Provider`
and `security::Bridge` into one provider helper, and give each framework its
own table row.
