#pragma once

#include <cstddef>
#include <cstdint>

using HDLE = unsigned long long;
using WSTR = char *;
using MsgPrcor = void (*)(unsigned long long, unsigned long long, unsigned long long);

struct XWINDOW
{
    unsigned int  width;
    unsigned int  height;
    WSTR          title;
    unsigned char sets;
};

struct XCOLORA
{
    unsigned char Red;
    unsigned char Green;
    unsigned char Blue;
    unsigned char Alpha;
};

constexpr unsigned char XWIN_NORMAL = 0;

constexpr unsigned long long MSG_CHAR = 0;
constexpr unsigned long long MSG_MOVE = 1;
constexpr unsigned long long MSG_LBUTTON = 2;
constexpr unsigned long long MSG_RBUTTON = 3;
constexpr unsigned long long MSG_MBUTTON = 4;
constexpr unsigned long long MSG_ROLLER = 5;
constexpr unsigned long long MSG_CRL = 6;
constexpr unsigned long long MSG_SPCHAR = 7;
constexpr unsigned long long MSG_RESIZE = 8;

constexpr unsigned char XWIN_SUPPORT_RESIZEABLE = 0x80;

struct in_addr
{
    unsigned int s_addr;
};

struct in6_addr
{
    unsigned char s6_addr[16];
};

using sa_family_t = unsigned short;
using in_port_t = unsigned short;
using socklen_t = unsigned int;

struct sockaddr
{
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in
{
    sa_family_t    sin_family;
    in_port_t      sin_port;
    in_addr        sin_addr;
    unsigned char  sin_zero[8];
};

struct sockaddr_in6
{
    sa_family_t    sin6_family;
    in_port_t      sin6_port;
    unsigned int   sin6_flowinfo;
    in6_addr       sin6_addr;
    unsigned int   sin6_scope_id;
};

struct pollfd
{
    int   fd;
    short events;
    short revents;
};

struct addrinfo
{
    int              ai_flags;
    int              ai_family;
    int              ai_socktype;
    int              ai_protocol;
    socklen_t        ai_addrlen;
    sockaddr        *ai_addr;
    char            *ai_canonname;
    addrinfo        *ai_next;
};

constexpr int AF_UNSPEC = 0;
constexpr int AF_INET = 2;
constexpr int AF_INET6 = 10;

constexpr int SOCK_STREAM = 1;

constexpr int SHUT_RD = 0;
constexpr int SHUT_WR = 1;
constexpr int SHUT_RDWR = 2;

constexpr short POLLIN = 0x0001;
constexpr short POLLOUT = 0x0004;
constexpr short POLLERR = 0x0008;
constexpr short POLLHUP = 0x0010;
constexpr short POLLNVAL = 0x0020;

constexpr int AI_PASSIVE = 0x0001;
constexpr int AI_CANONNAME = 0x0002;
constexpr int AI_NUMERICHOST = 0x0004;

constexpr int F_GETFL = 3;
constexpr int F_SETFL = 4;
constexpr int O_NONBLOCK = 04000;

constexpr int EAGAIN = 11;
constexpr int EWOULDBLOCK = EAGAIN;
constexpr int EINTR = 4;
constexpr int EINVAL = 22;
constexpr int EALREADY = 114;
constexpr int EINPROGRESS = 115;

extern "C" {
void     xapi_CreateWindow(HDLE *handle, XWINDOW *xwin);
void     xapi_SetWindowTitle(HDLE handle, WSTR str);
void     xapi_CloseWindow(HDLE handle);
void     xapi_SetIcon(HDLE handle, WSTR path);
void     xapi_GetWindowSize(HDLE handle, unsigned long long *width, unsigned long long *height);
void     xapi_DrawRect(HDLE handle, unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2, unsigned int color, bool fill);
void     xapi_DrawLine(HDLE handle, unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2, unsigned int color);
void     xapi_DrawText(HDLE handle, unsigned int x, unsigned int y, WSTR str, unsigned int size, unsigned int color);
void     xapi_DrawSWText(HDLE handle, unsigned int x, unsigned int y, WSTR str, unsigned int color);
unsigned long long xapi_CalcTextWidth(WSTR str, unsigned int size);
void     xapi_WriteBufferA(HDLE handle,
                           unsigned int x,
                           unsigned int y,
                           unsigned int width,
                           unsigned int height,
                           XCOLORA *buffer);
void     xapi_RefreshWindow(HDLE handle);
void     xapi_RefreshPartWindow(HDLE handle, unsigned int x1, unsigned int y1, unsigned int x2, unsigned int y2);
void     xapi_OutputSerial(WSTR str);
void     xapi_Sleep(unsigned long long ms);
void     SetMsgPrcor(HDLE handle, MsgPrcor func);

int      socket(int domain, int type, int protocol);
int      connect(int sockfd, const sockaddr *addr, socklen_t addrlen);
int      shutdown(int sockfd, int how);
int      close(int fd);
int      read(int fd, char *buf, unsigned long long len);
int      write(int fd, char *buf, unsigned long long len);
int      poll(pollfd *fds, unsigned long long nfds, unsigned long long timeout_ms);
int      fcntl(int fd, int cmd, unsigned long long arg);
int      getaddrinfo(const char *node, const char *service, const addrinfo *hints, addrinfo **res);
void     freeaddrinfo(addrinfo *res);
const char *gai_strerror(int errcode);
}

inline std::uint16_t htons(std::uint16_t value)
{
    return static_cast<std::uint16_t>((value << 8) | (value >> 8));
}

inline std::uint32_t htonl(std::uint32_t value)
{
    return ((value & 0x000000ffU) << 24) |
           ((value & 0x0000ff00U) << 8) |
           ((value & 0x00ff0000U) >> 8) |
           ((value & 0xff000000U) >> 24);
}

inline std::uint32_t xj380_rgba(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255)
{
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) |
           static_cast<std::uint32_t>(a);
}
