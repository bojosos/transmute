extern int python_loader_constructor_count;
extern int python_loader_missing_api(void);

__attribute__((constructor)) static void initialize(void) {
    python_loader_constructor_count += 100;
}

int python_loader_missing(void) {
    return python_loader_missing_api();
}
