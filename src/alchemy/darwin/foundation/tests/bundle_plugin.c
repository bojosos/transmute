/* The executable of AlchemyPlugin.bundle, loaded by bundle_fixture.c; plugin_factory is its
 * CFPlugIn factory (Info.plist CFPlugInFactories). */
int plugin_data = 7;
int plugin_answer(void) { return 35 + plugin_data; }
struct plugin_instance { int magic; };
static struct plugin_instance instance = {1234};
void *plugin_factory(const void *allocator, const void *type) { (void)allocator; (void)type; return &instance; }
