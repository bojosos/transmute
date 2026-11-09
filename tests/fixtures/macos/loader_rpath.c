// Lives only in loader-rpath/, which loader_app names in an LC_RPATH: a dlopen of
// its leaf name finds it through dyld's implicit @rpath.
int loader_rpath_value(void) {
    return 41;
}
