extern "C" int pr_info(const char *fmt, ...);

extern "C" int dlinker_dep_add(int a, int b);
extern "C" int dlinker_dep_value;

static int (*dep_add_ptr)(int, int) = dlinker_dep_add;
static int ctor_result = 0;

static void dlinker_test_ctor(void) __attribute__((constructor));
static void dlinker_test_ctor(void)
{
    ctor_result = dep_add_ptr(1, 2);
}

extern "C" __attribute__((visibility("default"))) int dlinker_test_export(void)
{
    return ctor_result + dlinker_dep_value;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlstart(void)
{
    return 0;
}

extern "C" __attribute__((used)) __attribute__((visibility("default"))) int dlmain(void)
{
    int direct_result = dlinker_dep_add(3, 4);
    int ptr_result = dep_add_ptr(5, 6);
    int export_result = dlinker_test_export();

    if (ctor_result != 40 || direct_result != 44 || ptr_result != 48 || export_result != 77) {
        pr_info("dlinker_test: failed ctor=%d direct=%d ptr=%d export=%d\n",
                ctor_result, direct_result, ptr_result, export_result);
        return -1;
    }

    pr_info("dlinker_test: passed ctor=%d direct=%d ptr=%d export=%d\n",
            ctor_result, direct_result, ptr_result, export_result);
    return 0;
}
