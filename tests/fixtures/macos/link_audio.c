/* Link-time stand-in for AudioUnit.framework: the loader resolves the import to
   Alchemy's provider at run time (the stub is never loaded). */
void* AudioComponentFindNext(void* component, const void* description) {
    (void)component;
    (void)description;
    return (void*)0;
}
