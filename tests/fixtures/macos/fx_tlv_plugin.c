/* fx_tlv's plugin: a second TLV image, dlopen'd after threads exist, with a
 * __thread_data value and a __thread_bss value. */
__thread long fx_tlv_plugin_data = 77;
__thread long fx_tlv_plugin_zero;
long fx_tlv_plugin_get(void) { return fx_tlv_plugin_data; }
void fx_tlv_plugin_set(long value) { fx_tlv_plugin_data = value; }
long fx_tlv_plugin_bss(void) { return fx_tlv_plugin_zero; }
long* fx_tlv_plugin_address(void) { return &fx_tlv_plugin_data; }
