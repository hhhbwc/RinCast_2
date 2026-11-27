"""PC 视频设置重建的静态门禁；不编译、不加载编码器，不证明运行时行为。"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
APP = (ROOT / "pc/streamer/src/app.cpp").read_text(encoding="utf-8")
SETTINGS = (ROOT / "pc/streamer/src/settings.h").read_text(encoding="utf-8")
SERIALIZATION = (ROOT / "pc/streamer/src/settings.cpp").read_text(encoding="utf-8")
SESSION = (ROOT / "pc/streamer/src/session/client_session.cpp").read_text(encoding="utf-8")
STREAM_CONFIG = (ROOT / "pc/streamer/src/stream/stream_config.h").read_text(encoding="utf-8")


def body(source, signature):
    start = source.index("{", source.index(signature)) + 1
    depth = 1
    for end in range(start, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end]
    raise AssertionError("函数体未闭合")


def trigger_keys():
    effects = body(APP, "std::string App::ApplySettingSideEffects(")
    live = body(effects, "if (auto session = m_server.ActiveSession())")
    match = re.fullmatch(
        r'\s*session->OnSettingsChanged\(changed\);\s*if\s*\((.*?)\)\s*'
        r'session->OnStreamProfileChanged\(\);\s*',
        re.sub(r"//[^\n]*", "", live), re.S)
    if not match:
        raise AssertionError("应在活动会话内保留一次设置通知及一个条件重建入口")
    expression = match[1]
    keys = re.findall(r'changed\.contains\("(\w+)"\)', expression)
    shape = re.sub(r'changed\.contains\("\w+"\)', "KEY", expression)
    if not re.fullmatch(r"\s*KEY(?:\s*\|\|\s*KEY)*\s*", shape):
        raise AssertionError("重建条件应仅由设置键的 OR 判断组成")
    if len(keys) != len(set(keys)):
        raise AssertionError("重建条件不应包含重复键")
    return set(keys)


class PcVideoSettingsGate(unittest.TestCase):
    def test_all_declared_video_keys_are_serialized_and_trigger_rebuild(self):
        video = SETTINGS.split("// Video", 1)[1].split("// Audio", 1)[0]
        keys = set(re.findall(r"^\s*(?:bool|int|float|std::string)\s+(\w+)\s*=", video, re.M))
        self.assertTrue(keys)
        # A86: streamProfile stays as a runtime carrier in Settings for EffectiveSettings,
        # deliberately omitted from ToJson/FromJson persistence.
        non_persisted = {"streamProfile"}
        self.assertTrue(non_persisted <= keys)
        for key in sorted(keys - non_persisted):
            self.assertIn(f'get("{key}", {key});', SERIALIZATION)
            self.assertIn(f'{{"{key}", {key}}}', SERIALIZATION)
        for key in sorted(non_persisted):
            self.assertNotIn(f'get("{key}",', SERIALIZATION)
            self.assertNotIn(f'{{"{key}",', SERIALIZATION)
        self.assertEqual(trigger_keys(), keys)

    def test_startup_pc_reads_and_codec_fallback_are_covered(self):
        desktop = body(SESSION, "void ClientSession::StartDesktop(")
        vr = body(SESSION, "void ClientSession::StartVr()")
        build_vr = body(STREAM_CONFIG, "inline VrStreamBuild BuildVrStreamConfig(")
        build_desktop = body(STREAM_CONFIG, "inline DesktopStreamConfig BuildDesktopStreamConfig(")
        resolve_desktop_fps = body(STREAM_CONFIG, "inline float ResolveDesktopFps(")
        self.assertIn("const Settings eff = EffectiveSettings(pc);", desktop)
        self.assertIn("const Settings eff = EffectiveSettings(pc);", vr)
        reads = set(re.findall(r"\bpc\.(\w+)", desktop + vr + build_vr + build_desktop + resolve_desktop_fps))
        # desktopInput 只控制提示和输入权限，不属于视频配置。
        self.assertEqual(reads - trigger_keys(), {"desktopInput"})
        codec = body(SESSION, "rc::VideoCodec ClientSession::ChooseCodec(")
        self.assertIn("SettingsStore::Get().Snapshot().codec", codec)
        self.assertIn("codec", trigger_keys())
        # PC tenBit 当前未被建流消费；保留此边界，避免把触发修复当成消费修复。
        self.assertNotIn("pc.tenBit", desktop + vr + build_vr + build_desktop)
        self.assertIn("cfg.tenBit = st.tenBit", build_desktop)
        self.assertIn("cfg.tenBit = st.tenBit", build_vr)

    def test_profile_and_cbr_share_one_presence_based_callsite(self):
        effects = body(APP, "std::string App::ApplySettingSideEffects(")
        self.assertEqual(effects.count("session->OnStreamProfileChanged();"), 1)
        # trigger_keys 同时约束完整条件为 contains 的 OR，不依赖开关值的真假。
        self.assertTrue({"qualityProfileCbr", "twoPassEncoding", "encodeScalePct", "l7Autoscale", "fillMode"} <= trigger_keys())

    def test_unrelated_and_empty_updates_do_not_trigger(self):
        for changed in ({}, {"streamAudio": False}, {"desktopInput": False},
                        {"pcName": "test", "preventSleep": True}, {"unknown": True}):
            with self.subTest(changed=changed):
                self.assertFalse(trigger_keys().intersection(changed))

    def test_existing_worker_dispatch_and_idle_guards_remain(self):
        changed_cb = body(SESSION, "void ClientSession::OnSettingsChanged(")
        self.assertNotRegex(changed_cb, r"\b(?:StartVr|StartDesktop|RestartVr)\s*\(")
        queued = body(SESSION, "void ClientSession::OnStreamProfileChanged()")
        self.assertIn('Enqueue(json{{"type", "_profile"}});', queued)
        self.assertNotIn("RestartVr()", queued)
        enqueue = body(SESSION, "void ClientSession::Enqueue(")
        self.assertIn("std::lock_guard lock(m_queueMutex);", enqueue)
        self.assertIn('"_profile"', enqueue)
        handler = body(SESSION, "void ClientSession::HandleMessage(")
        self.assertIn('if (type == "_profile") return ScheduleVideoRestart(true, true);', handler)
        self.assertIn('if (type == "_rebuild") return RestartVideoForProfile(m.value("vr", false), m.value("desktop", false));', handler)
        schedule = body(SESSION, "void ClientSession::ScheduleVideoRestart(")
        self.assertIn("std::lock_guard lock(m_queueMutex);", schedule)
        restart = body(SESSION, "void ClientSession::RestartVideoForProfile(")
        self.assertIn("if (hadVr) RestartVr();", restart)
        self.assertIn("monitors.push_back(d->Monitor().id);", restart)
        self.assertIn('if (!monitors.empty()) StartDesktop(json{{"monitors", monitors}});', restart)
        self.assertNotIn("StartVr()", restart)
        desktop = body(SESSION, "void ClientSession::StartDesktop(")
        self.assertIn("StopDesktop();", desktop)
        effects = body(APP, "std::string App::ApplySettingSideEffects(")
        self.assertNotRegex(effects, r"\b(?:StartVr|StartDesktop|RestartVr|RunAsync)\s*\(")


if __name__ == "__main__":
    unittest.main()
