__thread int alchemy_tls_value = 7;
int alchemy_tls_add(int increment) {
    alchemy_tls_value += increment;
    return alchemy_tls_value;
}
