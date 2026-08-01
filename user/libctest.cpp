#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures = 0;

static void out(const char *s)
{
    write(1, s, strlen(s));
}

static void expect(int cond, const char *name)
{
    if (cond) {
        out("通过 ");
        out(name);
        out("\n");
    } else {
        out("失败 ");
        out(name);
        out("\n");
        g_failures++;
    }
}

static int same(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static int vsnprintf_count_zero(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    return ret;
}

static int intcmp(const void *lhs, const void *rhs)
{
    int a = *(const int *)lhs;
    int b = *(const int *)rhs;
    return (a > b) - (a < b);
}

static int libctest_main_impl()
{
    out("libctest：开始\n");

    char mem[16];
    memset(mem, 'A', sizeof(mem));
    mem[15] = 0;
    expect(mem[0] == 'A' && mem[14] == 'A', "memset");

    memcpy(mem, "abcdef", 7);
    memmove(mem + 2, mem, 5);
    expect(memcmp(mem, "ababcde", 7) == 0, "memmove overlap forward");

    memcpy(mem, "abcdef", 7);
    memmove(mem, mem + 2, 5);
    expect(same(mem, "cdef"), "memmove overlap backward");

    memcpy(mem, "abcdef", 7);
    expect(memchr(mem, 'd', 6) == mem + 3, "memchr");
    bzero(mem, sizeof(mem));
    expect(mem[0] == 0 && mem[15] == 0, "bzero");

    expect(strnlen("abcdef", 3) == 3, "strnlen limited");
    expect(strnlen("abc", 8) == 3, "strnlen terminated");
    expect(strcasecmp("ToyBox", "toybox") == 0, "strcasecmp");
    expect(strncasecmp("ToyBox", "toycar", 3) == 0, "strncasecmp");
    expect(strspn("abc123", "abc") == 3, "strspn");
    expect(strcspn("abc123", "123") == 3, "strcspn");
    expect(strpbrk("abc123", "32") != NULL && *strpbrk("abc123", "32") == '2', "strpbrk");
    expect(index("abc", 'b') != NULL && *index("abc", 'b') == 'b', "index");
    expect(rindex("abca", 'a') != NULL && rindex("abca", 'a') == "abca" + 3, "rindex");

    char *dup = strdup("hello");
    expect(dup != NULL && same(dup, "hello"), "strdup");
    free(dup);

    char *ndup = strndup("hello", 3);
    expect(ndup != NULL && same(ndup, "hel"), "strndup");
    free(ndup);

    char *heap = (char *)malloc(70000);
    expect(heap != NULL, "malloc brk grow");
    if (heap != NULL) {
        heap[0] = 'x';
        heap[69999] = 'y';
        char *grown = (char *)realloc(heap, 90000);
        expect(grown != NULL && grown[0] == 'x' && grown[69999] == 'y', "realloc preserve");
        free(grown);
    }

    char *zeroed = (char *)calloc(16, 4);
    int all_zero = zeroed != NULL;
    for (int i = 0; zeroed != NULL && i < 64; i++) {
        if (zeroed[i] != 0) all_zero = 0;
    }
    expect(all_zero, "calloc zero");
    free(zeroed);

    expect(printf("") == 0 && fprintf(stdout, "") == 0 && dprintf(1, "") == 0, "stdio formatted empty");

    char fmtbuf[32];
    memset(fmtbuf, 'X', sizeof(fmtbuf));
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%5s", "ab") == 5 && same(fmtbuf, "   ab"), "snprintf string right pad");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%-5s", "ab") == 5 && same(fmtbuf, "ab   "), "snprintf string left pad");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%2s", "abcd") == 4 && same(fmtbuf, "abcd"), "snprintf string narrow width");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%-2s", "abcd") == 4 && same(fmtbuf, "abcd"),
           "snprintf string left narrow width");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "[%10s]", "hi") == 12 && same(fmtbuf, "[        hi]"),
           "snprintf string bracketed width");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%.3s", "abcdef") == 3 && same(fmtbuf, "abc"),
           "snprintf string precision");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%10.3s", "abcdef") == 10 && same(fmtbuf, "       abc"),
           "snprintf string width precision");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%-10.3s", "abcdef") == 10 && same(fmtbuf, "abc       "),
           "snprintf string left width precision");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%.*s", 2, "abcdef") == 2 && same(fmtbuf, "ab"),
           "snprintf string star precision");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%.*s", -1, "abcdef") == 6 && same(fmtbuf, "abcdef"),
           "snprintf string negative star precision");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%s", NULL) == 6 && same(fmtbuf, "(null)"), "snprintf null string");
    memset(fmtbuf, 'X', sizeof(fmtbuf));
    expect(snprintf(fmtbuf, 4, "abcdef") == 6 && same(fmtbuf, "abc") && fmtbuf[4] == 'X', "snprintf truncate literal");
    memset(fmtbuf, 'X', sizeof(fmtbuf));
    expect(snprintf(fmtbuf, 1, "abcdef") == 6 && fmtbuf[0] == '\0' && fmtbuf[1] == 'X',
           "snprintf truncate size one");
    memset(fmtbuf, 'X', sizeof(fmtbuf));
    expect(snprintf(fmtbuf, 0, "abcdef") == 6 && fmtbuf[0] == 'X', "snprintf count size zero");
    expect(snprintf(NULL, 0, "abcdef") == 6, "snprintf null size zero");
    expect(simple_snprintf(NULL, 0, "abcdef") == 6, "simple snprintf null size zero");
    expect(vsnprintf_count_zero("abcdef") == 6, "vsnprintf null size zero");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%3c", 'z') == 3 && same(fmtbuf, "  z"), "snprintf char right pad");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%-3c", 'z') == 3 && same(fmtbuf, "z  "), "snprintf char left pad");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%05d", 42) == 5 && same(fmtbuf, "00042"), "snprintf zero pad number");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%#x", 42) == 4 && same(fmtbuf, "0x2a"), "snprintf hex prefix");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%8llx", 0x2aULL) == 8 && same(fmtbuf, "      2a"), "snprintf long long width");
    expect(snprintf(fmtbuf, sizeof(fmtbuf), "%%") == 1 && same(fmtbuf, "%"), "snprintf percent literal");
    expect(fileno(stdin) == 0 && fileno(stdout) == 1 && fileno(stderr) == 2, "fileno std streams");
    expect(fflush(NULL) == 0 && fputs("", stdout) == 0, "stdio flush puts");

    expect(setenv("LIBCTEST_ENV", "one", 1) == 0 && same(getenv("LIBCTEST_ENV"), "one"), "setenv getenv");
    expect(setenv("LIBCTEST_ENV", "two", 0) == 0 && same(getenv("LIBCTEST_ENV"), "one"), "setenv no overwrite");
    expect(unsetenv("LIBCTEST_ENV") == 0 && getenv("LIBCTEST_ENV") == NULL, "unsetenv");
    expect(strtoull("0x2a", NULL, 0) == 42 && strtoll("-42", NULL, 10) == -42, "strto integer");

    int nums[] = {4, 1, 3, 2};
    qsort(nums, 4, sizeof(nums[0]), intcmp);
    int key = 3;
    int *found = (int *)bsearch(&key, nums, 4, sizeof(nums[0]), intcmp);
    expect(nums[0] == 1 && nums[3] == 4 && found != NULL && *found == 3, "qsort bsearch");

    gid_t groups[1] = {99};
    expect(getuid() == 0 && geteuid() == 0 && getegid() == getgid() && getgroups(1, groups) == 1 && groups[0] == 0,
           "uid gid stubs");
    expect(isatty(1) == 1 && ttyname(1) != NULL && same(ttyname(1), "/dev/tty"), "tty stubs");
    expect(S_ISREG(S_IFREG) && S_ISDIR(S_IFDIR) && (DEFFILEMODE & S_IRUSR), "stat macros");

    char tokbuf[] = "aa,bb,,cc";
    char *save = NULL;
    char *a = strtok_r(tokbuf, ",", &save);
    char *b = strtok_r(NULL, ",", &save);
    char *c = strtok_r(NULL, ",", &save);
    char *d = strtok_r(NULL, ",", &save);
    expect(a && b && c && d == NULL && same(a, "aa") && same(b, "bb") && same(c, "cc"), "strtok_r");

    expect(isalpha('A') && isalpha('z') && !isalpha('1'), "isalpha");
    expect(isdigit('7') && !isdigit('x'), "isdigit");
    expect(isspace(' ') && isspace('\n') && !isspace('x'), "isspace");
    expect(tolower('A') == 'a' && toupper('z') == 'Z', "case convert");
    expect(isxdigit('f') && isxdigit('F') && isxdigit('9') && !isxdigit('g'), "isxdigit");

    errno = ENOENT;
    expect(__errno_location() == &errno, "__errno_location");
    expect(same(strerror(errno), "No such file or directory"), "strerror");
    perror("libctest perror 示例");

    if (g_failures == 0) {
        out("libctest：完成\n");
        return 0;
    }

    out("libctest：失败\n");
    return 1;
}

extern "C" int libctest_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int libctest_main_cpp(int, char **, char **)
{
    return libctest_main_impl();
}
