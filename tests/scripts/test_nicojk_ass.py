import contextlib
from decimal import Decimal
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "nicojk_ass.py"
SPEC = importlib.util.spec_from_file_location("nicojk_ass", SCRIPT)
NICOJK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NICOJK)


class LocalCommentsTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.directory = Path(self.tempdir.name)

    def write(self, content, suffix="xml"):
        path = self.directory / ("comments." + suffix)
        path.write_text(content, encoding="utf-8-sig")
        return path

    def test_XMLの本文と属性を維持してマイクロ秒から振り直す(self):
        path = self.write('<packet><chat date="1700000001" date_usec="125000" '
                          'vpos="999999" mail="ue red">あ&amp;い</chat></packet>')
        result = NICOJK.rebase_file_vpos(NICOJK.read_comments_file(path),
                                       Decimal("1700000000.25"), 1700000010)
        chat = ET.fromstring(result).find("chat")
        self.assertEqual(chat.get("vpos"), "88")
        self.assertEqual(chat.get("mail"), "ue red")
        self.assertEqual(chat.text, "あ&い")

    def test_JKLの見出しコメントと複数行を読み込む(self):
        path = self.write('<!-- NicoJKログ -->\n'
                          '<chat date="100" vpos="5">先頭</chat>\n'
                          '<chat date="110">末尾</chat>\n', "jkl")
        root = NICOJK.read_comments_file(path)
        self.assertEqual(NICOJK.inspect_comments(root, 100, 110), (2, 100, 2))
        result = ET.fromstring(NICOJK.rebase_file_vpos(root, 100, 110))
        self.assertEqual([chat.get("vpos") for chat in result], ["0", "1000"])

    def test_noがないコメントだけに元順序の連番を補う(self):
        root = ET.fromstring('<packet><chat date="99">範囲外</chat>'
                             '<chat date="100" no="42">既存番号</chat>'
                             '<chat date="101">番号なし</chat></packet>')
        result = ET.fromstring(NICOJK.rebase_file_vpos(root, 100, 110))
        self.assertEqual([chat.get("no") for chat in result], ["42", "3"])

    def test_noなしXMLとJKLも実変換でASSに残る(self):
        converter = Path(os.environ.get("NICOJK_DANMAKU2ASS", ""))
        if not converter.is_file():
            converter = Path(__file__).resolve().parents[3] / "build_itest" / "exe_files" / "danmaku2ass.py"
        if not converter.is_file():
            self.skipTest("実変換器未配置。NICOJK_DANMAKU2ASSで指定できます")
        for suffix in ("xml", "jkl"):
            with self.subTest(suffix=suffix):
                body = '<chat date="101">番号なしのコメント</chat>'
                contents = '<packet>' + body + '</packet>' if suffix == "xml" else '<!-- NicoJK -->\n' + body
                path = self.write(contents, suffix)
                output = self.directory / (suffix + ".ass")
                argv = [str(SCRIPT), "--input", str(path), "--starttime", "100",
                        "--endtime", "110", "--width", "1280", "--height", "720",
                        "--output", str(output)]
                with mock.patch.object(sys, "argv", argv), \
                        mock.patch.object(NICOJK, "fetch_comments_xml", side_effect=AssertionError("API呼び出し禁止")), \
                        mock.patch.object(NICOJK, "find_danmaku2ass", return_value=str(converter)), \
                        contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exit:
                    NICOJK.main()
                self.assertEqual(exit.exception.code, 0)
                dialogue = [line for line in output.read_text(encoding="utf-8").splitlines()
                            if line.startswith("Dialogue:")]
                self.assertEqual(len(dialogue), 1)
                self.assertIn(",0:00:01.00,", dialogue[0])
                self.assertIn("番号なしのコメント", dialogue[0])

    def test_範囲の両端を残し丸める前に範囲外を除く(self):
        root = ET.fromstring('<packet>' + ''.join(
            '<chat date="100" date_usec="{}">{}</chat>'.format(usec, usec)
            for usec in (249999, 250000, 749999, 750000, 750001)) + '</packet>')
        result = ET.fromstring(NICOJK.rebase_file_vpos(root, "100.25", "100.75"))
        self.assertEqual([chat.text for chat in result], ["250000", "749999", "750000"])
        self.assertEqual([chat.get("vpos") for chat in result], ["0", "50", "50"])

    def test_検査は最古の時刻と範囲内件数を返す(self):
        root = ET.fromstring('<packet><chat date="110"/><chat date="100" '
                             'date_usec="500000"/><chat date="130"/></packet>')
        self.assertEqual(NICOJK.inspect_comments(root, 100, 110),
                         (3, Decimal("100.5"), 2))

    def test_無関係なXMLと空packetはコメントなしになる(self):
        for content in ('<settings><chat date="100"/></settings>', '<packet/>',
                        '<packet><nested><chat date="100"/></nested></packet>'):
            with self.subTest(content=content):
                root = NICOJK.read_comments_file(self.write(content))
                self.assertEqual(NICOJK.inspect_comments(root, 100, 110), (0, 0, 0))

    def test_不正なXMLと時刻を拒否する(self):
        for content in ('<packet>', '<packet><chat/></packet>',
                        '<packet><chat date="NaN"/></packet>',
                        '<packet><chat date="100" date_usec="1000000"/></packet>',
                        '<packet><chat date="100" date_usec="-1"/></packet>'):
            with self.subTest(content=content):
                with self.assertRaises((ET.ParseError, ValueError)):
                    NICOJK.read_comments_file(self.write(content))

    def test_検査コマンドの標準出力と終了コード(self):
        for content, code, expected in (
                ('<packet><chat date="100" date_usec="250000"/></packet>', 0, "1 100.25 1"),
                ('<packet/>', 101, "0 0 0"), ('<packet>', 1, "")):
            with self.subTest(content=content):
                result = subprocess.run([sys.executable, str(SCRIPT), "--inspect-input",
                                         str(self.write(content)), "--starttime", "100",
                                         "--endtime", "110"], capture_output=True, text=True)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_ファイル経路はAPIも既存の相対化も呼ばない(self):
        path = self.write('<packet><chat date="101" vpos="9999">本文</chat></packet>')
        output = self.directory / "result.ass"
        argv = [str(SCRIPT), "--input", str(path), "--starttime", "100",
                "--endtime", "110", "--width", "1280", "--height", "720",
                "--output", str(output)]

        def convert(xml_path, output_path, *args):
            chat = ET.parse(xml_path).getroot().find("chat")
            self.assertEqual(chat.get("vpos"), "100")
            Path(output_path).write_text("[Events]\n", encoding="utf-8")

        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(NICOJK, "fetch_comments_xml") as fetch, \
                mock.patch.object(NICOJK, "rebase_vpos") as rebase, \
                mock.patch.object(NICOJK, "run_danmaku2ass", side_effect=convert), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exit:
            NICOJK.main()
        self.assertEqual(exit.exception.code, 0)
        fetch.assert_not_called()
        rebase.assert_not_called()
        self.assertIn("[Events]", output.read_text(encoding="utf-8"))

    def test_存在しない入力ファイルは一般エラーになる(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--input",
                                 str(self.directory / "missing.xml"), "--starttime", "100",
                                 "--endtime", "110", "--width", "1280", "--height", "720",
                                 "--output", str(self.directory / "result.ass")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("読み込み失敗", result.stderr)

    def test_小数時刻を受け付け既存整数の表記を維持する(self):
        self.assertEqual(str(NICOJK.parse_timestamp("1700000000")), "1700000000")
        self.assertEqual(NICOJK.parse_timestamp("100.125"), Decimal("100.125"))
        for value in ("NaN", "inf", "abc"):
            with self.subTest(value=value), self.assertRaises(NICOJK.argparse.ArgumentTypeError):
                NICOJK.parse_timestamp(value)

    def test_APIは整数範囲を取得し小数指定時だけ追加調整する(self):
        source = ('<packet><chat date="101" vpos="1100">本文</chat>'
                  '<chat date="102" vpos="1200">末尾</chat></packet>')
        for start, end, expected in (("100", "102", ["100", "200"]),
                                     ("100.25", "101.75", ["75"])):
            with self.subTest(start=start):
                output = self.directory / "api.ass"
                argv = [str(SCRIPT), "--channel", "jk9", "--starttime", start,
                        "--endtime", end, "--width", "1280", "--height", "720",
                        "--output", str(output)]

                def convert(xml_path, output_path, *args):
                    chats = ET.parse(xml_path).getroot().findall("chat")
                    self.assertEqual([chat.get("vpos") for chat in chats], expected)
                    Path(output_path).write_text("[Events]\n", encoding="utf-8")

                with mock.patch.object(sys, "argv", argv), \
                        mock.patch.object(NICOJK, "fetch_comments_xml", return_value=source) as fetch, \
                        mock.patch.object(NICOJK, "rebase_vpos", wraps=NICOJK.rebase_vpos) as rebase, \
                        mock.patch.object(NICOJK, "run_danmaku2ass", side_effect=convert), \
                        contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exit:
                    NICOJK.main()
                self.assertEqual(exit.exception.code, 0)
                fetch.assert_called_once_with("jk9", 100, 102, NICOJK.DEFAULT_API_BASE)
                rebase.assert_called_once_with(source, 100)


if __name__ == "__main__":
    unittest.main()
