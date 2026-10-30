#include "./include/xposix/stdio.h"
#include "./include/xposix/string.h"
#include "./include/xposix/stdlib.h"
#include "./include/xposix/unistd.h"
#include "./include/xposix/ctype.h"

#define MAX_FAILURES 64

static int passed = 0;
static int failed = 0;
static const char *failure_list[MAX_FAILURES];
static int failure_count = 0;

static void check(const char *name, int ok)
{
    if (ok)
    {
        passed++;
    }
    else
    {
        if (failure_count < MAX_FAILURES)
            failure_list[failure_count++] = name;
        failed++;
    }
}

extern "C" int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    printf("=== XJ380 libc Test ===\n\n");

    // printf %d
    printf("[printf] Integer: %d\n", 42);
    printf("[printf] Negative: %d\n", -123);
    printf("[printf] Zero: %d\n", 0);

    // printf %u
    printf("\n[printf] Unsigned: %u\n", 4294967295u);

    // printf %x / %X
    printf("[printf] Hex lower: %x\n", 0xdeadbeef);
    printf("[printf] Hex upper: %X\n", 0xdeadbeef);
    printf("[printf] Hex with #: %#x\n", 0x1234);

    // printf %o
    printf("[printf] Octal: %o\n", 0755);

    // printf %s / %c / %p
    printf("\n[printf] String: %s\n", "Hello XJ380!");
    printf("[printf] Char: %c\n", 'A');
    printf("[printf] Pointer: %p\n", (void*)0x12345678);

    // printf width / padding
    printf("\n[printf] Width: %10d|\n", 42);
    printf("[printf] Zero-pad: %010d\n", 42);
    printf("[printf] Left-align: |%-10d|\n", 42);

    printf("\n--- Running checks ---\n\n");

    // 1. printf %d
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", 42);
        check("printf %d positive", strcmp(buf, "42") == 0);

        snprintf(buf, sizeof(buf), "%d", -123);
        check("printf %d negative", strcmp(buf, "-123") == 0);

        snprintf(buf, sizeof(buf), "%d", 0);
        check("printf %d zero", strcmp(buf, "0") == 0);
    }

    // 2. printf %u
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u", 4294967295u);
        check("printf %u max", strcmp(buf, "4294967295") == 0);

        snprintf(buf, sizeof(buf), "%u", 0);
        check("printf %u zero", strcmp(buf, "0") == 0);
    }

    // 3. printf %x / %X
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%x", 0xdeadbeef);
        check("printf %x lower", strcmp(buf, "deadbeef") == 0);

        snprintf(buf, sizeof(buf), "%X", 0xdeadbeef);
        check("printf %X upper", strcmp(buf, "DEADBEEF") == 0);

        snprintf(buf, sizeof(buf), "%#x", 0x1234);
        check("printf %#x prefix", strcmp(buf, "0x1234") == 0);
    }

    // 4. printf %o
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%o", 0755);
        check("printf %o", strcmp(buf, "755") == 0);
    }

    // 5. printf %s
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", "Hello");
        check("printf %s", strcmp(buf, "Hello") == 0);
    }

    // 6. printf %c
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%c", 'A');
        check("printf %c", strcmp(buf, "A") == 0);
    }

    // 7. printf width / padding
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%10d", 42);
        check("printf width right-pad", strcmp(buf, "        42") == 0);

        snprintf(buf, sizeof(buf), "%010d", 42);
        check("printf zero-pad", strcmp(buf, "0000000042") == 0);

        snprintf(buf, sizeof(buf), "|%-10d|", 42);
        check("printf left-align", strcmp(buf, "|42        |") == 0);
    }

    // 8. snprintf
    {
        char buf[32];
        int ret = snprintf(buf, sizeof(buf), "%d+%d=%d", 1, 2, 3);
        printf("[DEBUG] snprintf returned %d, buf='%s'\n", ret, buf);
        check("snprintf basic", strcmp(buf, "1+2=3") == 0 && ret == 5);
    }

    // 9. snprintf truncation
    {
        char buf[8];
        int ret = snprintf(buf, sizeof(buf), "Hello World");
        check("snprintf truncation", strcmp(buf, "Hello W") == 0 && ret == 11);
    }

    // 10. strlen
    check("strlen empty", strlen("") == 0);
    check("strlen hello", strlen("Hello") == 5);

    // 11. strcmp
    check("strcmp equal", strcmp("abc", "abc") == 0);
    check("strcmp less", strcmp("abc", "abd") < 0);
    check("strcmp greater", strcmp("abd", "abc") > 0);

    // 12. strcpy
    {
        char buf[32];
        strcpy(buf, "test");
        check("strcpy", strcmp(buf, "test") == 0);
    }

    // 13. strcat
    {
        char buf[32];
        strcpy(buf, "Hello");
        strcat(buf, " World");
        check("strcat", strcmp(buf, "Hello World") == 0);
    }

    // 14. strstr
    check("strstr found", strstr("Hello World", "World") != NULL);
    check("strstr not found", strstr("Hello World", "xyz") == NULL);

    // 15. memcpy
    {
        char src[] = "Hello";
        char dst[8] = {};
        memcpy(dst, src, 6);
        check("memcpy", strcmp(dst, "Hello") == 0);
    }

    // 16. memset
    {
        char buf[8];
        memset(buf, 'A', 5);
        buf[5] = '\0';
        check("memset", strcmp(buf, "AAAAA") == 0);
    }

    // 17. atoi
    check("atoi positive", atoi("12345") == 12345);
    check("atoi negative", atoi("-42") == -42);
    check("atoi zero", atoi("0") == 0);

    // 18. strtol
    {
        char *endptr;
        long v = strtol("0x1a", &endptr, 16);
        check("strtol hex", v == 26 && *endptr == '\0');

        v = strtol("42", &endptr, 10);
        check("strtol dec", v == 42 && *endptr == '\0');
    }

    // 19. malloc / free
    {
        int *p = (int *)malloc(sizeof(int) * 10);
        if (p != NULL)
        {
            p[0] = 42;
            p[9] = 99;
            check("malloc write", p[0] == 42 && p[9] == 99);
            free(p);
        }
        else
        {
            check("malloc non-null", 0);
        }
    }

    // 20. calloc
    {
        int *p = (int *)calloc(4, sizeof(int));
        if (p != NULL)
        {
            check("calloc zeroed", p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0);
            free(p);
        }
        else
        {
            check("calloc non-null", 0);
        }
    }

    // 21. realloc
    {
        int *p = (int *)malloc(sizeof(int) * 4);
        if (p != NULL)
        {
            p[0] = 1; p[1] = 2; p[2] = 3; p[3] = 4;
            int *q = (int *)realloc(p, sizeof(int) * 8);
            if (q != NULL)
            {
                check("realloc preserve", q[0] == 1 && q[3] == 4);
                free(q);
            }
            else
            {
                check("realloc non-null", 0);
            }
        }
        else
        {
            check("realloc malloc", 0);
        }
    }

    // 22. toupper / tolower
    check("toupper a->A", toupper('a') == 'A');
    check("tolower A->a", tolower('A') == 'a');
    check("toupper A unchanged", toupper('A') == 'A');

    // 23. isdigit / isalpha
    check("isdigit '5'", isdigit('5') != 0);
    check("isdigit 'a'", isdigit('a') == 0);
    check("isalpha 'a'", isalpha('a') != 0);
    check("isalpha '5'", isalpha('5') == 0);

    // Summary
    printf("\n=== Summary ===\n");
    printf("Total: %d, Passed: %d, Failed: %d\n", passed + failed, passed, failed);

    if (failed > 0)
    {
        printf("\n--- Failed tests ---\n");
        for (int i = 0; i < failure_count; i++)
        {
            printf("  [FAIL] %s\n", failure_list[i]);
        }
    }
    else
    {
        printf("All tests PASSED!\n");
    }

    return failed > 0 ? 1 : 0;
}
