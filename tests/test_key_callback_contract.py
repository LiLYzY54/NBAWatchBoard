#!/usr/bin/env python3
"""Static contract for the NBA board's key callback wiring.

The baseline demo owned key handling in ``main/main.c`` and needed a deep-sleep
wake guard there. This application instead receives keys in ``nba_app.c`` and
never deep-sleeps, so the guard does not apply; the property that still matters
is that the callback itself stays cheap: it runs in the button component's shared
``esp_timer`` task, where blocking or touching LVGL would stall every other timer.
"""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    return (ROOT / relative_path).read_text(encoding="utf-8")


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^;]*?\)\s*\{{", source)
    if not match:
        raise AssertionError(f"function not found: {name}")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start + 1:index]
    raise AssertionError(f"function is unterminated: {name}")


class KeyCallbackContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.app = read("main/nba_app.c")
        cls.main = read("main/main.c")

    def test_callback_only_enqueues(self) -> None:
        body = function_body(self.app, "nba_app_on_key")
        self.assertIn("xQueueSend(", body)
        # 阻塞、慢操作与 UI 操作都不允许出现在按键回调里。
        for forbidden in ("vTaskDelay", "xSemaphoreTake", "bsp_lvgl_lock",
                          "lv_", "nba_api_fetch", "esp_http_client"):
            self.assertNotIn(forbidden, body,
                             f"nba_app_on_key must not call {forbidden}")

    def test_main_delegates_key_handling_to_the_app(self) -> None:
        # main.c 只做初始化;按键注册发生在应用层,避免再次出现两套按键语义。
        self.assertNotIn("bsp_button_init(", self.main)
        self.assertIn("nba_app_start()", self.main)


if __name__ == "__main__":
    unittest.main()
