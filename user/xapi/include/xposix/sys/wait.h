#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define WNOHANG 1

#define WIFEXITED(status) (((status) & 0x7f) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

int wait(int *status);
int waitpid(int pid, int *status, int options);

#ifdef __cplusplus
}
#endif
