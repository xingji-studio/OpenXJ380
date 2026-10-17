"""Execute the production PTY callbacks with host-side lock/scheduler stubs."""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def function(source: str, name: str) -> str:
    match = re.search(r"^static [^\n]*\b" + name + r"\([^;]*?\)\n\{", source, re.M)
    if match is None:
        raise AssertionError(f"Missing production function {name}")
    end = source.index("\n}", match.end()) + 2
    return source[match.start():end]


@unittest.skipUnless(shutil.which("g++"), "requires a host C++ compiler")
class PtyInputTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        source = (ROOT / "driver/fs/vfs/pty.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/pty.h").read_text(encoding="utf-8")
        header = re.sub(r"^#(?:include|pragma).*\n", "", header, flags=re.M)
        constants = "\n".join(re.findall(r"^static constexpr uint8_t PTY_INPUT_.*", source, re.M))
        functions = "\n\n".join(function(source, name) for name in (
            "pty_termios_default", "pts_data_available", "pts_write_inner",
            "pty_cc_matches", "pty_echo_bytes", "pty_echo_control", "pty_echo_char", "pty_flush_input",
            "pty_input_byte", "ptmx_write", "pts_read", "pty_set_termios", "pts_poll",
        ))
        program = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
using vfs_node_t = void *;
using spin_t = int;
using errno_t = int;
struct llist_header { void *next; void *prev; };
static int lock_count = 0, sent_signal = 0, sent_group = 0;
static bool pending_signal = false;
static void spin_lock(spin_t *lock) { assert(*lock == 0); *lock = 1; ++lock_count; }
static void spin_unlock(spin_t *lock) { assert(*lock == 1); *lock = 0; --lock_count; }
static void *get_current_task() { return nullptr; }
static bool signals_pending_quick(void *) { return pending_signal; }
static void scheduler_yield() { assert(lock_count == 0); assert(!"unexpected blocking operation"); }
static void signal_send_group(int group, int sig) {
    assert(lock_count == 0); sent_group = group; sent_signal = sig;
}
template<class... T> static void unused(T...) {}
#define UNUSED(...) unused(__VA_ARGS__)
#define MIN(a,b) std::min((a),(b))
#define VFS_STATUS_FAILED (-1)
#define ONLCR 4
#define SIGINT 2
#define SIGQUIT 3
#define SIGTSTP 20
#define EPOLLIN 1
#define EPOLLOUT 4
#define EPOLLHUP 16
'''
        program += f'\n#include "{(ROOT / "include/tty.h").as_posix()}"\n'
        program += header + "\n" + constants + "\n" + functions
        program += r'''
static pty_handle_t pair;
static uint8_t master[PTY_BUFF_SIZE], slave[PTY_BUFF_SIZE];
static void reset() {
    pair = {}; pair.master_buffer = master; pair.slave_buffer = slave;
    pair.master_fds = pair.slave_fds = 1; pair.ctrl_pgid = 42;
    pty_termios_default(&pair.term);
    sent_signal = sent_group = 0; pending_signal = false;
}
static void send(const std::string &s) { assert(ptmx_write(&pair, s.data(), 0, s.size()) == s.size()); }
static std::string echo() { return std::string((char *)master, pair.ptr_master); }
static std::string receive(size_t count = PTY_BUFF_SIZE) {
    uint8_t out[PTY_BUFF_SIZE]; size_t n = pts_read(&pair, out, 0, count);
    assert(n <= sizeof(out)); return std::string((char *)out, n);
}
int main(int argc, char **argv) {
    assert(argc == 2); reset(); std::string test = argv[1];
    if (test == "canonical_echo") {
        send("echo OK"); assert(echo() == "echo OK");
        assert(!(pts_poll(&pair, EPOLLIN) & EPOLLIN));
        send("\r"); assert(echo() == "echo OK\r\n");
        assert(receive() == "echo OK\n");
    } else if (test == "password") {
        pair.term.c_lflag &= ~ECHO;
        send("secrex\177t\n"); assert(echo().empty()); assert(receive() == "secret\n");
        pair.term.c_lflag |= ECHONL;
        send("hidden\n"); assert(echo() == "\r\n"); assert(receive() == "hidden\n");
    } else if (test == "readline_raw") {
        pair.term.c_lflag &= ~(ICANON | ECHO | ISIG); pair.term.c_iflag = 0;
        const std::string keys("\033[A\177\003\004\0", 7);
        send(keys); assert(echo().empty()); assert(receive() == keys);
    } else if (test == "erase_kill") {
        send("old\nabc\177d"); assert(echo() == "old\r\nabc\b \bd");
        send("\025z\n"); assert(receive() == "old\n"); assert(receive() == "z\n");
        send("ok\n\177\025x\n"); assert(receive() == "ok\n"); assert(receive() == "x\n");
    } else if (test == "eof") {
        send("\004"); assert(pts_poll(&pair, EPOLLIN) & EPOLLIN); assert(receive().empty());
        assert(!(pts_poll(&pair, EPOLLIN) & EPOLLIN));
        send("ab\004next\n"); assert(receive(1) == "a"); assert(receive(1) == "b");
        assert(receive() == "next\n");
        send("\004\004x\n"); assert(receive().empty()); assert(receive().empty()); assert(receive() == "x\n");
    } else if (test == "nul_and_disabled_cc") {
        send(std::string("x\0y", 3)); assert(pts_data_available(&pair) == 0);
        send("\n"); assert(receive() == std::string("x\0y\n", 4));
        pair.term.c_cc[VEOF] = 0; pair.term.c_cc[VINTR] = 0;
        send(std::string("\003\004\0\n", 4)); assert(receive() == std::string("\003\004\0\n", 4));
    } else if (test == "signals") {
        send("unfinished\003"); assert(sent_group == 42 && sent_signal == SIGINT);
        assert(pair.ptr_slave == 0 && echo() == "^C");
        send("\034"); assert(sent_signal == SIGQUIT);
        send("\032"); assert(sent_signal == SIGTSTP);
        pair.term.c_lflag |= NOFLSH; send("keep"); size_t output = pair.ptr_master;
        send("\003"); assert(pair.ptr_slave == 4 && pair.ptr_master == output + 2);
        pair.term.c_lflag &= ~ISIG; send("\003\n"); assert(receive() == "keep\003\n");
    } else if (test == "interrupted_io") {
        pending_signal = true; uint8_t out = 0;
        assert(pts_read(&pair, &out, 0, 1) == (size_t)-EINTR);
        pair.term.c_lflag = 0; pair.ptr_slave = PTY_BUFF_SIZE;
        assert(ptmx_write(&pair, "x", 0, 1) == (size_t)-EINTR);
    } else if (test == "mode_change") {
        send("partial\004"); termios raw = pair.term; raw.c_lflag &= ~(ICANON | ECHO);
        pty_set_termios(&pair, &raw, false); assert(receive() == "partial");
        send("typeahead"); termios canon = raw; canon.c_lflag |= ICANON;
        pty_set_termios(&pair, &canon, false); assert(receive() == "typeahead");
        send("discard\n"); pty_set_termios(&pair, &canon, true);
        assert(pair.ptr_slave == 0 && pts_data_available(&pair) == 0);
    } else if (test == "long_line") {
        pair.term.c_lflag &= ~ECHO;
        send(std::string(PTY_BUFF_SIZE + 100, 'x')); assert(pair.ptr_slave == PTY_BUFF_SIZE - 1);
        send("\177y\n"); std::string line = receive();
        assert(line.size() == PTY_BUFF_SIZE && line.substr(line.size() - 2) == "y\n");
    } else if (test == "echo_backpressure") {
        pair.ptr_master = PTY_BUFF_SIZE - 1; int sig = 0;
        assert(!pty_input_byte(&pair, '\n', &sig)); assert(pair.ptr_slave == 0);
        pair.ptr_master = 0; assert(pty_input_byte(&pair, '\n', &sig));
        assert(echo() == "\r\n"); assert(receive() == "\n");
        send("a"); pair.ptr_master = PTY_BUFF_SIZE - 2;
        assert(!pty_input_byte(&pair, 127, &sig)); assert(pair.ptr_slave == 1);
        send("\003"); assert(pair.ptr_slave == 0 && sent_signal == SIGINT);
    } else if (test == "hangup_and_zero_read") {
        uint8_t out = 0; assert(pts_read(&pair, &out, 0, 0) == 0);
        send("tail"); pair.master_fds = 0;
        assert(receive() == "tail"); assert(receive().empty());
        pair.slave_fds = 0; assert(ptmx_write(&pair, "x", 0, 1) == (size_t)-EIO);
        assert(ptmx_write(&pair, "", 0, 0) == 0);
    } else { assert(!"unknown test"); }
    assert(lock_count == 0);
}
'''
        path = Path(cls.tmp.name) / "pty_input.cpp"
        cls.binary = Path(cls.tmp.name) / "pty_input"
        path.write_text(program, encoding="utf-8")
        subprocess.run([
            "g++", "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(path), "-o", str(cls.binary),
        ], check=True)

    def test_terminal_input_behaviors(self):
        for case in (
            "canonical_echo", "password", "readline_raw", "erase_kill", "eof", "nul_and_disabled_cc",
            "signals", "interrupted_io", "mode_change", "long_line", "echo_backpressure", "hangup_and_zero_read",
        ):
            with self.subTest(case=case):
                subprocess.run([str(self.binary), case], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
