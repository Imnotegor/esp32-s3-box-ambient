import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("status", Path(__file__).with_name("status.py"))
status = importlib.util.module_from_spec(spec)
spec.loader.exec_module(status)


class StatusTests(unittest.TestCase):
    def data(self):
        return {"snapshot": {"received_at_unix": 10000, "measurement": {
            "node_id": "box3", "temperature_mc": 23450,
            "humidity_milli_percent": 45600, "interval_secs": 3600}}}

    def test_fresh(self):
        text = status.render(self.data(), 10060)
        self.assertIn("23.4°C 46% 1m", text)
        self.assertNotIn("stale", text)

    def test_stale_and_offline_cache(self):
        self.assertIn("stale", status.render(self.data(), 16000))
        cache = self.data()
        cache["last_error"] = "unavailable"
        self.assertIn("cache", status.render(cache, 10100))

    def test_rejects_tmux_injection_and_future_clock(self):
        data = self.data()
        data["snapshot"]["measurement"]["temperature_mc"] = "#[bg=red]"
        self.assertEqual(status.render(data, 10100), "#[fg=#6272a4]BOX --")
        self.assertFalse(status.valid_snapshot(self.data()["snapshot"], 9000))

    def test_server_weather_shared_with_box_and_cache_marker(self):
        cache = self.data()
        cache["snapshot"]["dashboard"] = {
            "weather_ok": True, "weather_temp_mc": 15000, "weather_feels_mc": 12000,
            "weather_kind": 1, "weather_age_secs": 100, "weather_stale": False}
        cache["last_success_unix"] = 10000
        self.assertEqual(status.render_weather(cache, 10100), "⛅ +15°C +12°C")
        cache["last_error"] = "unavailable"
        self.assertTrue(status.render_weather(cache, 10100).endswith("~"))
        cache["snapshot"]["dashboard"]["weather_temp_mc"] = "#[bg=red]"
        self.assertEqual(status.render_weather(cache, 10100), "")

    def test_missing(self):
        self.assertEqual(status.render({}, 10000), "#[fg=#6272a4]BOX --")


if __name__ == "__main__":
    unittest.main()
