import unittest
from unittest.mock import Mock, call, patch

from pexpect_utils import DEFAULT_CHUNK_DELAY, send_line, send_text


class SerialPacingTests(unittest.TestCase):
    @patch("pexpect_utils.time.sleep")
    def test_command_tail_is_a_separate_burst(self, sleep):
        child = Mock()
        send_line(child, "readlink sym.txt")
        self.assertEqual(
            child.send.call_args_list, [call("readlink"), call(" sym.txt"), call("\n")]
        )
        self.assertEqual(sleep.call_args_list, [call(DEFAULT_CHUNK_DELAY)] * 3)

    @patch("pexpect_utils.time.sleep")
    def test_long_commands_preserve_every_character(self, sleep):
        child = Mock()
        command = "printf '%s\\n' 'serial transport'" * 100
        send_line(child, command)
        bursts = [arguments.args[0] for arguments in child.send.call_args_list]
        self.assertEqual("".join(bursts), command + "\n")
        self.assertTrue(all(0 < len(burst) <= 8 for burst in bursts))
        self.assertEqual(sleep.call_count, len(bursts))

    @patch("pexpect_utils.time.sleep")
    def test_heredoc_text_does_not_add_a_newline(self, sleep):
        child = Mock()
        text = "first line\nsecond line\nEOF\n"
        send_text(child, text)
        bursts = [arguments.args[0] for arguments in child.send.call_args_list]
        self.assertEqual("".join(bursts), text)
        self.assertEqual(sleep.call_count, len(bursts))

    @patch("pexpect_utils.time.sleep")
    def test_explicit_chunk_and_delay_overrides(self, sleep):
        child = Mock()
        send_text(child, "abcdefgh", chunk_size=3, delay=0)
        self.assertEqual(
            child.send.call_args_list, [call("abc"), call("def"), call("gh")]
        )
        sleep.assert_not_called()

    @patch("pexpect_utils.time.sleep")
    def test_empty_text_sends_nothing(self, sleep):
        child = Mock()
        send_text(child, "")
        child.send.assert_not_called()
        sleep.assert_not_called()


if __name__ == "__main__":
    unittest.main()
