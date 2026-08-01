extern "C" int pr_info(const char *fmt, ...);

extern "C" {
__attribute__((visibility("default"))) int dlinker_dep_value = 37;
}

extern "C" __attribute__((visibility("default"))) int dlinker_dep_add(int a, int b)
{
    return a + b + dlinker_dep_value;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlstart(void)
{
    return 0;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlmain(void)
{
    pr_info("dlinker_dep: dlmain\n");
    return 0;
}
