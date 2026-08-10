import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class UserImageCandidateLoaderTests(unittest.TestCase):
    def test_candidate_prepare_keeps_loading_detached_and_incomplete(self):
        source = (ROOT / "kernel" / "user" / "user.cpp").read_text(encoding="utf-8")
        prepare = source.split("int user_image_prepare_elf", 1)[1].split("uint64_t parse_elf_file", 1)[0]

        self.assertIn("user_image_candidate_address_space_owner(candidate, &owner)", prepare)
        self.assertIn("switch_page_directory(candidate->pagedir)", prepare)
        self.assertIn("load_user_elf_image", prepare)
        self.assertNotIn("user_image_candidate_mark_prepared", prepare)
        self.assertNotIn("user_image_commit_locked", prepare)
        self.assertNotIn("pcb_t", prepare)


if __name__ == "__main__":
    unittest.main()
