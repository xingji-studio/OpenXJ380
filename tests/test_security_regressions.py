import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class CliInputSecurityTests(unittest.TestCase):
    def test_cli_input_is_bounded_and_passwords_disable_echo(self):
        cli = (ROOT / "user" / "cli_shell.cpp").read_text(encoding="utf-8")
        kernel = (ROOT / "kernel" / "syscall" / "xapi" / "xtui.cpp").read_text(encoding="utf-8")

        self.assertIn("enter_syscall(XAPI_INPUT, (uint64_t)buffer, capacity, flags", cli)
        self.assertIn("read_line(password, sizeof(password), XAPI_INPUT_NO_ECHO)", cli)
        self.assertIn("read_line(line, sizeof(line))", cli)
        self.assertIn("if (buffer == nullptr || capacity == 0) return", cli)
        self.assertIn("char username[64] = {}", cli)
        self.assertIn("char password[64] = {}", cli)
        self.assertIn("char line[256] = {}", cli)
        self.assertIn("capacity == 0 || capacity > XAPI_USER_STRING_MAX", kernel)
        self.assertIn("if (input_len >= capacity) input_len = capacity - 1", kernel)


class PartitionBoundsSecurityTests(unittest.TestCase):
    def test_gpt_and_mbr_entries_are_validated_before_registration(self):
        source = (ROOT / "driver" / "fs" / "partition.cpp").read_text(encoding="utf-8")

        self.assertIn("gpt->first_usable_lba > gpt->last_usable_lba", source)
        self.assertIn("entry->starting_lba < gpt->first_usable_lba", source)
        self.assertIn("entry->ending_lba > gpt->last_usable_lba", source)
        self.assertIn("partition_lba_range_is_valid(entry->starting_lba", source)
        self.assertIn("ending_lba = starting_lba + sector_count - 1", source)
        self.assertIn("partition_lba_range_is_valid(starting_lba, ending_lba", source)


if __name__ == "__main__":
    unittest.main()
