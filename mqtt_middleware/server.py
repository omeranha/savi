from __future__ import annotations

import argparse
import asyncio
import json
import logging
import math
import os
import re
from contextlib import AsyncExitStack
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode, urlparse
from urllib.request import Request, urlopen

from amqtt.broker import Broker
from amqtt.client import MQTTClient
from amqtt.mqtt.constants import QOS_1
from dotenv import load_dotenv

LOGGER = logging.getLogger("savi.middleware")

TELEMETRY_TOPIC = "savi/telemetry"
ALERT_TOPIC = "savi/alert"
STATUS_TOPIC = "savi/gateway"
OFFLINE_TOPIC = "savi/gateway/offline"
ID_REQUEST_TOPIC = "savi/id/request"
ID_RESPONSE_TOPIC = "savi/id/response"

MAC_PATTERN = re.compile(r"^[0-9a-fA-F]{2}(?::[0-9a-fA-F]{2}){5}$")
LOOPBACK_HOSTS = frozenset({"localhost", "127.0.0.1", "::1"})
WILDCARD_BINDS = frozenset({"0.0.0.0", "::"})

TELEMETRY_NUMBER_FIELDS = ("temperatura", "acc_x", "acc_y", "acc_z", "gyro_x", "gyro_y", "gyro_z")
TELEMETRY_BYTE_FIELDS = ("bpm", "spo2")
TELEMETRY_COLUMNS = (*TELEMETRY_NUMBER_FIELDS, *TELEMETRY_BYTE_FIELDS)
# Sent only by a device that has a battery gauge, so they are written when present.
BATTERY_COLUMN = "bateria"
# PostgreSQL stamps the row when the gateway's clock has not synced yet.
TIMESTAMP_COLUMN = "atualizado_em"


class SupabaseError(Exception):
    pass


@dataclass(frozen=True)
class Settings:
    bind: str
    port: int
    log_level: str
    supabase_url: str
    supabase_key: str = field(repr=False)


class SupabaseClient:
    """The bracelet MAC is the device identity; PostgreSQL owns esp_id."""

    def __init__(self, url: str, api_key: str, timeout: float = 10.0) -> None:
        parsed = urlparse(url)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise ValueError("SUPABASE_URL must be an http(s) URL")
        if parsed.scheme == "http" and parsed.hostname not in LOOPBACK_HOSTS:
            raise ValueError("SUPABASE_URL must use HTTPS except for localhost")
        if not api_key:
            raise ValueError("SUPABASE_SERVICE_ROLE_KEY must not be empty")
        self.host = parsed.netloc
        self.rest_url = f"{url.rstrip('/')}/rest/v1"
        self.api_key = api_key
        self.timeout = timeout

    def _request(self, method: str, resource: str, query: dict[str, str] | None, payload: dict[str, Any] | None, prefer: str) -> Any:
        url = f"{self.rest_url}/{resource}"
        if query:
            url = f"{url}?{urlencode(query)}"
        headers = {"apikey": self.api_key, "Authorization": f"Bearer {self.api_key}", "Accept": "application/json"}
        body = None
        if payload is not None:
            headers["Content-Type"] = "application/json"
            headers["Prefer"] = prefer
            body = json.dumps(payload, allow_nan=False).encode("utf-8")

        LOGGER.debug("Supabase %s %s", method, url)
        try:
            with urlopen(Request(url, data=body, headers=headers, method=method), timeout=self.timeout) as response:
                raw = response.read()
        except HTTPError as exc:
            details = exc.read(2048).decode("utf-8", errors="replace")
            raise SupabaseError(f"HTTP {exc.code} on {method} {resource}: {details}") from exc
        except (URLError, TimeoutError, OSError) as exc:
            raise SupabaseError(f"{method} {resource} could not reach {self.host}: {exc}") from exc

        if not raw:
            return None
        try:
            return json.loads(raw)
        except json.JSONDecodeError as exc:
            raise SupabaseError(f"{method} {resource} returned invalid JSON") from exc

    def _rows(self, method: str, resource: str, *, query: dict[str, str] | None = None, payload: dict[str, Any] | None = None, expected: int | None = None, prefer: str = "return=representation") -> list[dict[str, Any]]:
        body = self._request(method, resource, query, payload, prefer)
        if not isinstance(body, list) or any(not isinstance(row, dict) for row in body):
            raise SupabaseError(f"{method} {resource} did not return a list of rows")
        if expected is not None and len(body) != expected:
            raise SupabaseError(f"{method} {resource} touched {len(body)} rows, expected {expected}")
        return body

    def write_telemetry(self, telemetry: dict[str, Any]) -> int:
        """Upsert the row for this bracelet and return the esp_id Supabase holds."""
        payload = {"mac": telemetry["mac"], **{column: telemetry[column] for column in TELEMETRY_COLUMNS}}
        for optional in (BATTERY_COLUMN, TIMESTAMP_COLUMN):
            if telemetry.get(optional) is not None:
                payload[optional] = telemetry[optional]
        # ON CONFLICT (mac) DO UPDATE: the row appears by itself the first time a
        # bracelet reports, and em_uso keeps whatever a human set on it.
        rows = self._rows("POST", "braceletes", query={"on_conflict": "mac", "select": "esp_id"}, payload=payload, expected=1, prefer="return=representation,resolution=merge-duplicates")
        return require_esp_id(rows[0])

    def write_alert(self, esp_id: int, alert: dict[str, Any]) -> None:
        payload = {"esp_id": esp_id, "tipo": alert["tipo"], "valor": alert["valor"]}
        self._rows("POST", "alertas", payload=payload, expected=1)

    def write_gateway_status(self, status: dict[str, Any]) -> None:
        """One row per gateway, keyed by its MAC, marked connected."""
        payload = {
            "mac": status["mac"],
            "local": status["local"],
            "braceletes": status["braceletes"],
            "is_connected": True,
        }
        for optional in (BATTERY_COLUMN, TIMESTAMP_COLUMN):
            if status.get(optional) is not None:
                payload[optional] = status[optional]
        self._rows("POST", "gateways", query={"on_conflict": "mac", "select": "gateway_id"}, payload=payload, expected=1, prefer="return=representation,resolution=merge-duplicates")

    def mark_gateway_offline(self, mac: str) -> bool:
        """False when Supabase has never heard of this gateway."""
        rows = self._rows("PATCH", "gateways", query={"mac": f"eq.{mac}"}, payload={"is_connected": False, "braceletes": 0})
        return len(rows) > 0

    def mark_all_gateways_offline(self) -> int:
        """Called at startup: anything still flagged connected is stale."""
        rows = self._rows("PATCH", "gateways", query={"is_connected": "eq.true"}, payload={"is_connected": False, "braceletes": 0})
        return len(rows)

    def bracelet_id_for(self, mac: str) -> int:
        """esp_id of this bracelet, inserting its row when Supabase has never seen it."""
        esp_id = self.find_bracelet_id(mac)
        if esp_id is not None:
            return esp_id
        try:
            rows = self._rows("POST", "braceletes", query={"select": "esp_id"}, payload={"mac": mac}, expected=1)
        except SupabaseError:
            # A telemetry upsert for the same bracelet may have won the race.
            esp_id = self.find_bracelet_id(mac)
            if esp_id is None:
                raise
            return esp_id
        LOGGER.info("Created the braceletes row for %s", mac)
        return require_esp_id(rows[0])

    def find_bracelet_id(self, mac: str) -> int | None:
        rows = self._rows("GET", "braceletes", query={"mac": f"eq.{mac}", "select": "esp_id"})
        if not rows:
            return None
        if len(rows) > 1:
            raise SupabaseError(f"{mac} matches {len(rows)} braceletes rows, mac must be unique")
        return require_esp_id(rows[0])


def require_esp_id(row: dict[str, Any]) -> int:
    value = row.get("esp_id")
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise ValueError(f"Supabase returned esp_id={value!r}, expected a positive integer")
    return value


def normalize_mac(value: Any) -> str:
    if not isinstance(value, str) or MAC_PATTERN.fullmatch(value) is None:
        raise ValueError("mac must be six hexadecimal octets separated by colons")
    return value.lower()


def parse_json(payload: bytes) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("payload must be UTF-8 JSON") from exc
    if not isinstance(value, dict):
        raise ValueError("payload must be a JSON object")
    return value


def require_mac(payload: dict[str, Any]) -> str:
    payload["mac"] = normalize_mac(payload.get("mac"))
    return payload["mac"]


def require_number(payload: dict[str, Any], key: str) -> int | float:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{key} must be a number")
    if isinstance(value, float) and not math.isfinite(value):
        raise ValueError(f"{key} must be finite")
    return value


def require_byte(payload: dict[str, Any], key: str) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 255:
        raise ValueError(f"{key} must be an integer from 0 to 255")
    return value


def require_optional_percent(payload: dict[str, Any], key: str) -> int | None:
    """A bracelet without a battery gauge omits the key instead of inventing a value."""
    value = payload.get(key)
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= 100:
        raise ValueError(f"{key} must be an integer from 0 to 100 or absent")
    return value


def require_timestamp(payload: dict[str, Any], key: str) -> str:
    value = payload.get(key)
    if not isinstance(value, str):
        raise ValueError(f"{key} must be an ISO-8601 timestamp")
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError as exc:
        raise ValueError(f"{key} must be an ISO-8601 timestamp") from exc
    if parsed.utcoffset() is None:
        raise ValueError(f"{key} must include a UTC offset")
    return value


def require_optional_timestamp(payload: dict[str, Any], key: str) -> str | None:
    """The gateway leaves this out until NTP lands, and PostgreSQL stamps the row."""
    if payload.get(key) is None:
        return None
    return require_timestamp(payload, key)


def require_count(payload: dict[str, Any], key: str) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"{key} must be an integer of 0 or more")
    return value


def require_text(payload: dict[str, Any], key: str, limit: int) -> str:
    value = payload.get(key)
    if not isinstance(value, str) or not 1 <= len(value) <= limit:
        raise ValueError(f"{key} must be a string of 1 to {limit} characters")
    return value


def validate_telemetry(payload: dict[str, Any]) -> dict[str, Any]:
    require_mac(payload)
    require_optional_timestamp(payload, TIMESTAMP_COLUMN)
    for key in TELEMETRY_NUMBER_FIELDS:
        require_number(payload, key)
    for key in TELEMETRY_BYTE_FIELDS:
        require_byte(payload, key)
    require_optional_percent(payload, BATTERY_COLUMN)
    return payload


def validate_alert(payload: dict[str, Any]) -> dict[str, Any]:
    require_mac(payload)
    alert_type = payload.get("tipo")
    if not isinstance(alert_type, str) or not 1 <= len(alert_type) <= 32:
        raise ValueError("tipo must be a non-empty string of at most 32 characters")
    require_number(payload, "valor")
    return payload


def validate_gateway_status(payload: dict[str, Any]) -> dict[str, Any]:
    require_mac(payload)
    require_text(payload, "local", 64)
    require_count(payload, "braceletes")
    require_optional_percent(payload, BATTERY_COLUMN)
    require_optional_timestamp(payload, TIMESTAMP_COLUMN)
    return payload


class Middleware:
    def __init__(self, client: MQTTClient, supabase: SupabaseClient) -> None:
        self.client = client
        self.supabase = supabase
        # Cache of the esp_id PostgreSQL assigned to each MAC, for the alertas FK.
        self.esp_ids: dict[str, int] = {}
        self.handlers = {
            TELEMETRY_TOPIC: self.handle_telemetry,
            ALERT_TOPIC: self.handle_alert,
            STATUS_TOPIC: self.handle_gateway_status,
            OFFLINE_TOPIC: self.handle_gateway_offline,
            ID_REQUEST_TOPIC: self.handle_id_request,
        }

    async def run(self) -> None:
        topics = sorted(self.handlers)
        await self.client.subscribe([(topic, QOS_1) for topic in topics])
        LOGGER.info("Subscribed to %s", ", ".join(topics))
        cleared = await asyncio.to_thread(self.supabase.mark_all_gateways_offline)
        if cleared:
            LOGGER.info("Cleared is_connected on %d gateway row(s) left over from the last run", cleared)
        while True:
            message = await self.client.deliver_message()
            await self.handle(message.topic, message.data)

    async def handle(self, topic: str, payload: bytes) -> None:
        handler = self.handlers.get(topic)
        if handler is None:
            LOGGER.debug("Ignoring %d bytes on unhandled topic %s", len(payload), topic)
            return
        try:
            await handler(parse_json(payload))
        except ValueError as exc:
            LOGGER.warning("Rejected %d-byte payload on %s: %s", len(payload), topic, exc)
        except SupabaseError as exc:
            LOGGER.error("Supabase call failed while handling %s: %s", topic, exc)
        except Exception:
            LOGGER.exception("Unexpected failure while handling %s, middleware stays up", topic)

    async def handle_id_request(self, payload: dict[str, Any]) -> None:
        """A bracelet with no esp_id asked, by way of its gateway."""
        mac = require_mac(payload)
        esp_id = self.esp_ids.get(mac)
        if esp_id is None:
            esp_id = await asyncio.to_thread(self.supabase.bracelet_id_for, mac)
            self.esp_ids[mac] = esp_id
        response = json.dumps({"mac": mac, "esp_id": esp_id}, separators=(",", ":")).encode("utf-8")
        await self.client.publish(ID_RESPONSE_TOPIC, response, qos=QOS_1)
        LOGGER.info("Gave esp_id=%d to bracelet %s", esp_id, mac)

    async def handle_telemetry(self, payload: dict[str, Any]) -> None:
        telemetry = validate_telemetry(payload)
        mac = telemetry["mac"]
        esp_id = await asyncio.to_thread(self.supabase.write_telemetry, telemetry)
        if self.esp_ids.get(mac) != esp_id:
            self.esp_ids[mac] = esp_id
            LOGGER.info("Bracelet %s is streaming telemetry as esp_id=%d", mac, esp_id)
        LOGGER.debug("Telemetry %s temp=%s bpm=%s spo2=%s bateria=%s", mac, telemetry["temperatura"], telemetry["bpm"], telemetry["spo2"], telemetry.get(BATTERY_COLUMN))

    async def handle_alert(self, payload: dict[str, Any]) -> None:
        alert = validate_alert(payload)
        mac = alert["mac"]
        esp_id = self.esp_ids.get(mac)
        if esp_id is None:
            esp_id = await asyncio.to_thread(self.supabase.bracelet_id_for, mac)
            self.esp_ids[mac] = esp_id
        await asyncio.to_thread(self.supabase.write_alert, esp_id, alert)
        LOGGER.warning("Alert %s from %s (esp_id=%d) with valor=%s", alert["tipo"], mac, esp_id, alert["valor"])

    async def handle_gateway_status(self, payload: dict[str, Any]) -> None:
        status = validate_gateway_status(payload)
        await asyncio.to_thread(self.supabase.write_gateway_status, status)
        LOGGER.info("Gateway %s at %s is connected with %s bracelet(s), battery %s", status["mac"], status["local"], status["braceletes"], status.get(BATTERY_COLUMN))

    async def handle_gateway_offline(self, payload: dict[str, Any]) -> None:
        """The broker's last will for a gateway that stopped answering keepalives."""
        mac = require_mac(payload)
        known = await asyncio.to_thread(self.supabase.mark_gateway_offline, mac)
        if known:
            LOGGER.warning("Gateway %s dropped off the broker, marked disconnected", mac)
        else:
            LOGGER.warning("Gateway %s dropped off the broker but has no row yet", mac)


def broker_config(bind: str, port: int) -> dict[str, Any]:
    return {
        "listeners": {"default": {"type": "tcp", "bind": f"{bind}:{port}"}},
        "plugins": {"amqtt.plugins.authentication.AnonymousAuthPlugin": {"allow_anonymous": True}},
    }


def client_host(bind: str) -> str:
    return "127.0.0.1" if bind in WILDCARD_BINDS else bind


async def run_server(settings: Settings) -> None:
    supabase = SupabaseClient(settings.supabase_url, settings.supabase_key)
    broker = Broker(broker_config(settings.bind, settings.port))
    client = MQTTClient()

    async with AsyncExitStack() as stack:
        await broker.start()
        stack.push_async_callback(broker.shutdown)
        LOGGER.info("Broker accepting anonymous clients on mqtt://%s:%d", settings.bind, settings.port)
        await client.connect(f"mqtt://{client_host(settings.bind)}:{settings.port}/")
        stack.push_async_callback(client.disconnect)
        await Middleware(client, supabase).run()


def env_int(name: str, default: int) -> int:
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        return int(raw)
    except ValueError:
        raise SystemExit(f"{name} must be an integer, got {raw!r}") from None


def parse_settings(argv: list[str] | None = None) -> Settings:
    parser = argparse.ArgumentParser(description="Run the SAVI MQTT broker and Supabase middleware.")
    parser.add_argument("--bind", default=os.environ.get("SAVI_MQTT_BIND", "0.0.0.0"), help="interface to bind, env SAVI_MQTT_BIND (default: 0.0.0.0)")
    parser.add_argument("--port", type=int, default=env_int("SAVI_MQTT_PORT", 1883), help="MQTT TCP port, env SAVI_MQTT_PORT (default: 1883)")
    parser.add_argument("--log-level", default=os.environ.get("SAVI_LOG_LEVEL", "INFO").upper(), choices=["DEBUG", "INFO", "WARNING", "ERROR"], help="middleware log level, env SAVI_LOG_LEVEL (default: INFO)")
    args = parser.parse_args(argv)

    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")

    supabase_url = os.environ.get("SUPABASE_URL", "").strip()
    supabase_key = os.environ.get("SUPABASE_SERVICE_ROLE_KEY", "").strip()
    if not supabase_url or not supabase_key:
        parser.error("SUPABASE_URL and SUPABASE_SERVICE_ROLE_KEY must be set in the environment or .env (see .env.example)")

    return Settings(args.bind, args.port, args.log_level, supabase_url, supabase_key)


def load_environment(env_file: Path | None = None) -> None:
    load_dotenv(env_file or Path(__file__).with_name(".env"), override=False)


def configure_logging(level: str) -> None:
    logging.basicConfig(level=logging.WARNING, format="%(asctime)s %(levelname)-7s %(name)s | %(message)s")
    LOGGER.setLevel(level)
    logging.getLogger("amqtt").setLevel(logging.INFO if level == "DEBUG" else logging.WARNING)
    logging.getLogger("transitions").setLevel(logging.WARNING)


def main() -> None:
    load_environment()
    settings = parse_settings()
    configure_logging(settings.log_level)
    LOGGER.info("Starting SAVI middleware: supabase=%s", urlparse(settings.supabase_url).netloc)
    try:
        asyncio.run(run_server(settings))
    except KeyboardInterrupt:
        LOGGER.info("Interrupted, middleware stopped")
    except SupabaseError as exc:
        raise SystemExit(f"Supabase is unreachable: {exc}") from exc
    except ValueError as exc:
        raise SystemExit(f"Invalid configuration: {exc}") from exc


if __name__ == "__main__":
    main()
