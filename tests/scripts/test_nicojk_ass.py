import contextlib
from decimal import Decimal
import importlib.util
import io
import json
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


class TimeMapTests(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)
        self.directory = Path(self.tempdir.name)

    def write_map(self, entries):
        path = self.directory / "timemap.json"
        path.write_text(json.dumps(entries), encoding="utf-8-sig")
        return path

    def write_comments(self):
        path = self.directory / "comments.xml"
        path.write_text('<packet><chat date="1001" no="42" user_id="same">同じ本文</chat>'
                        '<chat date="1003" no="99">後半</chat></packet>', encoding="utf-8")
        return path

    def argv(self, extra):
        return [str(SCRIPT), "--starttime", "1000", "--endtime", "1010",
                "--width", "1280", "--height", "720", "--output",
                str(self.directory / "result.ass")] + extra

    def run_main(self, argv, fetch=None):
        converted = []

        def convert(xml_path, output_path, *args):
            converted.extend(ET.parse(xml_path).getroot().findall("chat"))
            self.assertEqual(args[-1], NICOJK.DEFAULT_DURATION)
            Path(output_path).write_text("[Events]\n", encoding="utf-8")

        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(NICOJK, "fetch_comments_xml", side_effect=fetch) as api, \
                mock.patch.object(NICOJK, "run_danmaku2ass", side_effect=convert) as converter, \
                contextlib.redirect_stderr(io.StringIO()) as log, self.assertRaises(SystemExit) as exit:
            NICOJK.main()
        return exit.exception.code, converted, api, converter, log.getvalue()

    def test_対応表をfloatの4つ組として読む(self):
        result = NICOJK.read_timemap(self.write_map([[0, 1.25, 0, 2.5], [1.25, 3, 2.5, 4]]))
        self.assertEqual(result[0], (0.0, 1.25, 0.0, 2.5))
        self.assertEqual(NICOJK.read_timemap(self.write_map([])), [])


    def test_半開区間で写して隙間と範囲外を捨て属性を保つ(self):
        timemap = NICOJK.read_timemap(self.write_map([[1, 2, 10, 12], [3, 4, 15, 16]]))
        xml = '<packet>' + ''.join('<chat vpos="{}" no="42" date="1000" mail="ue">{}</chat>'.format(
            value, value) for value in (99, 100, 199, 200, 250, 300, 399, 400)) + '<chat>位置なし</chat></packet>'
        result = ET.fromstring(NICOJK.apply_timemap(xml, timemap)).findall("chat")
        self.assertEqual([chat.text for chat in result], ["100", "199", "300", "399"])
        self.assertEqual([chat.get("vpos") for chat in result], ["1000", "1198", "1500", "1599"])
        self.assertTrue(all(chat.get("no") == "42" and chat.get("date") == "1000"
                            and chat.get("mail") == "ue" for chat in result))

    def test_隣接境界のコメントを二重に写さない(self):
        timemap = NICOJK.read_timemap(self.write_map([[0, 1, 0, 2], [1, 2, 5, 6]]))
        result = ET.fromstring(NICOJK.apply_timemap(
            '<packet><chat vpos="-1"/><chat vpos="100"/><chat vpos="200"/></packet>', timemap))
        self.assertEqual([chat.get("vpos") for chat in result], ["500"])
        self.assertEqual(len(ET.fromstring(NICOJK.apply_timemap('<packet><chat vpos="0"/></packet>', []))), 0)

    def test_統合は時刻順で既存番号と重複を残す(self):
        reference = '<packet><chat vpos="300" no="42" user_id="same">同じ本文</chat>' \
                    '<chat vpos="100" no="7">基準</chat></packet>'
        target = '<packet><chat vpos="100" no="42" user_id="same">同じ本文</chat></packet>'
        result = ET.fromstring(NICOJK.merge_comments(reference, target)).findall("chat")
        self.assertEqual([chat.get("vpos") for chat in result], ["100", "100", "300"])
        self.assertEqual([chat.get("no") for chat in result], ["7", "42", "42"])
        self.assertEqual(sum(chat.text == "同じ本文" for chat in result), 2)

    def test_ファイルと対応表だけならAPIを使わず配置は1回(self):
        code, chats, api, converter, _ = self.run_main(self.argv([
            "--input", str(self.write_comments()), "--timemap", str(self.write_map([[0, 2, 2, 6]]))]),
            fetch=AssertionError("API呼び出し禁止"))
        self.assertEqual(code, 0)
        self.assertEqual([chat.get("vpos") for chat in chats], ["400"])
        self.assertEqual(chats[0].get("no"), "42")
        api.assert_not_called()
        converter.assert_called_once()

    def test_APIも既存の相対化の後で対応表を使う(self):
        source = '<packet><chat date="1001" vpos="1100" no="42">本文</chat></packet>'
        timemap = self.write_map([[0, 2, 10, 12]])
        with mock.patch.object(NICOJK, "rebase_vpos", wraps=NICOJK.rebase_vpos) as rebase:
            code, chats, api, converter, _ = self.run_main(
                self.argv(["--channel", "jk9", "--timemap", str(timemap)]),
                fetch=lambda *args: source)
        self.assertEqual(code, 0)
        self.assertEqual([chat.get("vpos") for chat in chats], ["1100"])
        api.assert_called_once_with("jk9", 1000, 1010, NICOJK.DEFAULT_API_BASE)
        rebase.assert_called_once_with(source, 1000)
        converter.assert_called_once()

    def test_統合する対象も既存API経路を使い配置前に混ぜる(self):
        target = ('<packet><chat date="201" vpos="500" no="42" user_id="same">同じ本文</chat>'
                  '<chat date="203" vpos="700" no="88">対象後半</chat></packet>')
        argv = self.argv(["--input", str(self.write_comments()), "--timemap", str(self.write_map([[0, 10, 2, 12]])),
                          "--merge-channel", "jk1", "--merge-starttime", "200", "--merge-endtime", "210"])
        with mock.patch.object(NICOJK, "rebase_vpos", wraps=NICOJK.rebase_vpos) as rebase:
            code, chats, api, converter, _ = self.run_main(argv, fetch=lambda *args: target)
        self.assertEqual(code, 0)
        self.assertEqual([chat.get("vpos") for chat in chats], ["100", "300", "300", "500"])
        self.assertEqual([chat.get("no") for chat in chats], ["42", "42", "88", "99"])
        self.assertEqual(sum(chat.text == "同じ本文" for chat in chats), 2)
        api.assert_called_once_with("jk1", 200, 210, NICOJK.DEFAULT_API_BASE)
        rebase.assert_called_once_with(target, 200)
        converter.assert_called_once()

    def test_対象局未対応だけは警告して基準だけを使う(self):
        argv = self.argv(["--input", str(self.write_comments()), "--timemap",
                          str(self.write_map([[0, 10, 0, 10]])), "--merge-channel", "jk1",
                          "--merge-starttime", "200", "--merge-endtime", "210"])
        code, chats, api, converter, log = self.run_main(argv, fetch=SystemExit(100))
        self.assertEqual(code, 0)
        self.assertEqual([chat.get("vpos") for chat in chats], ["100", "300"])
        self.assertIn("未対応", log)
        converter.assert_called_once()
        code, _, _, converter, _ = self.run_main(argv, fetch=SystemExit(1))
        self.assertEqual(code, 1)
        converter.assert_not_called()




    def test_対応表で写したコメントも実変換で指定時刻に出る(self):
        converter = Path(os.environ.get("NICOJK_DANMAKU2ASS", ""))
        if not converter.is_file():
            converter = Path(__file__).resolve().parents[3] / "build_itest" / "exe_files" / "danmaku2ass.py"
        if not converter.is_file():
            self.skipTest("実変換器未配置。NICOJK_DANMAKU2ASSで指定できます")
        argv = self.argv(["--input", str(self.write_comments()), "--timemap",
                          str(self.write_map([[0, 2, 2, 4]]))])
        with mock.patch.object(sys, "argv", argv), \
                mock.patch.object(NICOJK, "fetch_comments_xml", side_effect=AssertionError("API呼び出し禁止")), \
                mock.patch.object(NICOJK, "find_danmaku2ass", return_value=str(converter)), \
                contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as exit:
            NICOJK.main()
        self.assertEqual(exit.exception.code, 0)
        dialogues = [line for line in (self.directory / "result.ass").read_text().splitlines()
                     if line.startswith("Dialogue:")]
        self.assertEqual(len(dialogues), 1)
        self.assertIn(",0:00:03.00,", dialogues[0])
        self.assertIn("同じ本文", dialogues[0])


if __name__ == "__main__":
    unittest.main()
