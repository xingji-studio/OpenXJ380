#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("[dyn-hello] 参数数=%d argv0=%s\n", argc, argc > 0 ? argv[0] : "(null)");
    printf("[dyn-hello] AT_BASE=0x%lx AT_ENTRY=0x%lx AT_PHDR=0x%lx\n",
           getauxval(AT_BASE),
           getauxval(AT_ENTRY),
           getauxval(AT_PHDR));

    errno = 0;
    long pagesz = sysconf(_SC_PAGESIZE);
    printf("[dyn-hello] sysconf(_SC_PAGESIZE)=%ld 错误码=%d\n", pagesz, errno);

    static const char write_msg[] = "[dyn-hello] write 系统调用可用\n";
    write(STDOUT_FILENO, write_msg, sizeof(write_msg) - 1);
    return 0;
}
