int util_sum_squares(int count) {
    int total = 0;
    for (int i = 1; i <= count; i++) total += i * i;
    return total;
}
