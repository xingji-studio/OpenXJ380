#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include "xapi/include/libsys.h"
#include "xapi/include/xposix/syscall_ret.h"

static constexpr uint64_t SYS_UTIMENSAT_NR = 280;

static void ok(const char *msg, unsigned long len)
{
    write(1, msg, len);
}

static void fail(const char *msg, unsigned long len)
{
    write(2, msg, len);
}

#define OK(msg) ok(msg, sizeof(msg) - 1)
#define FAIL(msg) fail(msg, sizeof(msg) - 1)

static int posixdemo_main_impl()
{
    OK("posixdemo：开始\n");

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        OK("clock_gettime 通过\n");
    } else {
        FAIL("clock_gettime 失败\n");
    }

    const char *path = "/posixdemo.tmp";
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        FAIL("open 失败\n");
        return 1;
    }

    const char payload[] = "来自 xapi posix 的测试文本\n";
    if (write(fd, payload, sizeof(payload) - 1) < 0) {
        FAIL("write 失败\n");
        close(fd);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) == 0) {
        OK("fstat 通过\n");
    } else {
        FAIL("fstat 失败\n");
    }

    struct timespec file_times[2] = {{1585591680, 0}, {1585591680, 0}};
    long raw_utime = __xposix_ret(enter_syscall(SYS_UTIMENSAT_NR, fd, 0, (uint64_t)file_times, 0, 0, 0));
    if (raw_utime == 0) {
        OK("utimensat fd/null 通过\n");
    } else {
        FAIL("utimensat fd/null 失败\n");
    }

    lseek(fd, 0, SEEK_SET);
    char buf[64] = {};
    if (read(fd, buf, sizeof(buf) - 1) >= 0) {
        OK("read 通过\n");
    } else {
        FAIL("read 失败\n");
    }
    close(fd);

    struct iovec iov[2];
    const char left[] = "writev ";
    const char right[] = "通过\n";
    iov[0].iov_base = (void *)left;
    iov[0].iov_len = sizeof(left) - 1;
    iov[1].iov_base = (void *)right;
    iov[1].iov_len = sizeof(right) - 1;
    writev(1, iov, 2);

    int pipefd[2];
    if (pipe(pipefd) == 0) {
        const char byte = 'x';
        write(pipefd[1], &byte, 1);
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(pipefd[0], &readfds);
        struct timeval timeout = {0, 0};
        if (select(pipefd[0] + 1, &readfds, NULL, NULL, &timeout) >= 0) {
            OK("pipe/select 通过\n");
        }
        close(pipefd[0]);
        close(pipefd[1]);
    } else {
        FAIL("pipe 失败\n");
    }

    DIR *dir = opendir("/");
    if (dir != NULL) {
        readdir(dir);
        closedir(dir);
        OK("dirent 通过\n");
    } else {
        FAIL("opendir 失败\n");
    }

    void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem != MAP_FAILED) {
        mprotect(mem, 4096, PROT_READ);
        munmap(mem, 4096);
        OK("mmap 通过\n");
    } else {
        FAIL("mmap 失败\n");
    }

    unlink(path);
    OK("posixdemo：完成\n");
    return 0;
}

extern "C" int posixdemo_main_cpp(int argc, char *argv[], char *envp[]) asm("_Z4mainiPPcS0_");
extern "C" int posixdemo_main_cpp(int, char **, char **)
{
    return posixdemo_main_impl();
}
