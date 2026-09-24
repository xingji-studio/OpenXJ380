import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class LogConfigTests(unittest.TestCase):
    def test_serial_config_is_validated_stored_and_used_by_default_filter(self):
        header = (ROOT / "include" / "syscall" / "pxapi.h").read_text(encoding="utf-8")
        proto = (ROOT / "include" / "proto.hpp").read_text(encoding="utf-8")
        serial = (ROOT / "driver" / "serial" / "serial_port.cpp").read_text(encoding="utf-8")

        self.assertIn("#define LOG_CONFIG_CATEGORY_MASK", header)
        self.assertIn("#define LOG_CONFIG_VALID_MASK", header)
        self.assertIn('extern "C" bool serial_logging_allowed() __attribute__((weak));', serial)
        self.assertNotIn("serial_logging_allowed", proto)
        self.assertIn("flags & LOG_CONFIG_VALID_MASK", serial)
        self.assertIn("__atomic_store_n(&serial_log_config", serial)
        self.assertIn("__atomic_load_n(&serial_log_config", serial)
        self.assertIn("serial_log_config_get() & LOG_CONFIG_RECORD_LOGS", serial)

    def test_syscall_dispatch_implements_set_and_get(self):
        syscall = (ROOT / "kernel" / "syscall" / "syscall.cpp").read_text(encoding="utf-8")

        self.assertIn('case SXAH_SET_LOG_CONFIG: return "sxah_set_log_config";', syscall)
        self.assertIn('case SXAH_GET_LOG_CONFIG: return "sxah_get_log_config";', syscall)
        self.assertIn("(regs->rdi & ~LOG_CONFIG_VALID_MASK) != 0", syscall)
        self.assertIn("serial_log_config_set(regs->rdi);", syscall)
        self.assertIn("case SXAH_GET_LOG_CONFIG: regs->rax = serial_log_config_get(); break;", syscall)


if __name__ == "__main__":
    unittest.main()
