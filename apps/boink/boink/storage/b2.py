from __future__ import annotations

import base64
import hashlib
import json
import logging
import random
import threading
import time
import urllib.parse
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any

import requests

from ..config import BoinkConfig, required_env
from ..models import ObjectRecord

RETRYABLE_STATUS = {408, 429, 500, 502, 503, 504}
PERMANENT_STATUS = {400, 403, 404}
EXPIRED_TOKEN_CODES = {"expired_auth_token", "bad_auth_token"}
log = logging.getLogger("boink.b2")


class B2Error(RuntimeError):
    def __init__(self, operation: str, status: int | None, code: str, retryable: bool):
        self.operation = operation
        self.status = status
        self.code = code
        self.retryable = retryable
        super().__init__(
            f"{operation} failed ({'network' if status is None else 'HTTP ' + str(status)}, "
            f"code={code or 'unknown'}, retryable={retryable})"
        )


def response_code(response: requests.Response) -> str:
    try:
        value = response.json()
    except ValueError:
        return ""
    return str(value.get("code", "")) if isinstance(value, dict) else ""


def is_retryable_response(response: requests.Response) -> bool:
    code = response_code(response)
    return response.status_code in RETRYABLE_STATUS or code in EXPIRED_TOKEN_CODES


class B2Transport:
    """The single authoritative B2 client for Boink.

    Upload URLs are cached per thread. A retryable endpoint failure discards only
    that thread's URL and obtains a new URL before the file is attempted again.
    """

    def __init__(
        self,
        *,
        key_id: str,
        application_key: str,
        bucket_name: str,
        max_attempts: int = 6,
        session: requests.Session | Any | None = None,
        sleep: Callable[[float], None] = time.sleep,
        jitter: Callable[[], float] = random.random,
    ):
        self.key_id = key_id
        self.application_key = application_key
        self.bucket_name = bucket_name
        self.max_attempts = max_attempts
        self._provided_session = session
        self._sleep = sleep
        self._jitter = jitter
        self._auth_lock = threading.Lock()
        self._thread = threading.local()
        self._auth: dict[str, Any] | None = None
        self.retry_events = 0

    @property
    def session(self) -> requests.Session | Any:
        if self._provided_session is not None:
            return self._provided_session
        value = getattr(self._thread, "http_session", None)
        if value is None:
            value = requests.Session()
            self._thread.http_session = value
        return value

    @classmethod
    def from_env(cls, config: BoinkConfig) -> B2Transport:
        return cls(
            key_id=required_env("B2_KEY_ID"),
            application_key=required_env("B2_APPLICATION_KEY"),
            bucket_name=config.b2_bucket_name,
            max_attempts=config.max_attempts,
        )

    def _backoff(self, attempt: int, response: requests.Response | None = None) -> None:
        retry_after = ""
        if response is not None:
            retry_after = response.headers.get("Retry-After", "")
        try:
            delay = min(120.0, max(0.0, float(retry_after)))
        except ValueError:
            delay = min(60.0, 2.0 ** (attempt - 1)) + self._jitter()
        if not retry_after:
            delay = min(60.0, 2.0 ** (attempt - 1)) + self._jitter()
        self._sleep(delay)

    def authorize(self, *, force: bool = False) -> dict[str, Any]:
        with self._auth_lock:
            if self._auth is not None and not force:
                return self._auth
            basic = base64.b64encode(
                f"{self.key_id}:{self.application_key}".encode()
            ).decode("ascii")
            last: B2Error | None = None
            for attempt in range(1, self.max_attempts + 1):
                try:
                    response = self.session.request(
                        "GET",
                        "https://api.backblazeb2.com/b2api/v2/b2_authorize_account",
                        headers={"Authorization": f"Basic {basic}"},
                        timeout=60,
                    )
                except requests.RequestException as exc:
                    last = B2Error("B2 authorization", None, type(exc).__name__, True)
                    if attempt == self.max_attempts:
                        raise last from exc
                    self.retry_events += 1
                    log.warning(
                        "B2 authorization network retry %s/%s",
                        attempt,
                        self.max_attempts,
                    )
                    self._backoff(attempt)
                    continue
                if response.ok:
                    payload = response.json()
                    self._validate_authorization(payload)
                    self._auth = payload
                    return payload
                retryable = is_retryable_response(response)
                last = B2Error(
                    "B2 authorization",
                    response.status_code,
                    response_code(response),
                    retryable,
                )
                if not retryable or attempt == self.max_attempts:
                    raise last
                self.retry_events += 1
                log.warning(
                    "B2 authorization HTTP %s retry %s/%s",
                    response.status_code,
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt, response)
            assert last is not None
            raise last

    def _validate_authorization(self, payload: dict[str, Any]) -> None:
        for name in ("authorizationToken", "apiUrl", "downloadUrl"):
            if not payload.get(name):
                raise RuntimeError(f"B2 authorization response is missing {name}")
        allowed = payload.get("allowed") or {}
        restricted_name = allowed.get("bucketName")
        if restricted_name and restricted_name != self.bucket_name:
            raise RuntimeError("B2 key is restricted to a different bucket")

    @property
    def capabilities(self) -> set[str]:
        return set((self.authorize().get("allowed") or {}).get("capabilities", []))

    def require_capabilities(self, capabilities: Iterable[str]) -> None:
        missing = set(capabilities) - self.capabilities
        if missing:
            raise RuntimeError(f"B2 key lacks required capabilities: {sorted(missing)}")

    @property
    def bucket_id(self) -> str:
        auth = self.authorize()
        allowed = auth.get("allowed") or {}
        if allowed.get("bucketId") and allowed.get("bucketName") == self.bucket_name:
            return str(allowed["bucketId"])
        self.require_capabilities({"listBuckets"})
        payload = self._api(
            "b2_list_buckets", {"accountId": auth["accountId"]}, "B2 bucket discovery"
        )
        matches = [
            item
            for item in payload.get("buckets", [])
            if item.get("bucketName") == self.bucket_name
        ]
        if len(matches) != 1:
            raise RuntimeError("Expected B2 bucket was not found uniquely")
        return str(matches[0]["bucketId"])

    def _api(self, method: str, body: dict[str, Any], operation: str) -> dict[str, Any]:
        last: B2Error | None = None
        for attempt in range(1, self.max_attempts + 1):
            auth = self.authorize()
            try:
                response = self.session.request(
                    "POST",
                    f"{auth['apiUrl']}/b2api/v2/{method}",
                    headers={"Authorization": auth["authorizationToken"]},
                    json=body,
                    timeout=60,
                )
            except requests.RequestException as exc:
                last = B2Error(operation, None, type(exc).__name__, True)
                if attempt == self.max_attempts:
                    raise last from exc
                self.retry_events += 1
                log.warning(
                    "%s network retry %s/%s",
                    operation,
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt)
                continue
            if response.ok:
                return response.json()
            code = response_code(response)
            retryable = is_retryable_response(response)
            if response.status_code == 401 and code in EXPIRED_TOKEN_CODES:
                self.authorize(force=True)
            last = B2Error(operation, response.status_code, code, retryable)
            if not retryable or attempt == self.max_attempts:
                raise last
            self.retry_events += 1
            log.warning(
                "%s HTTP %s retry %s/%s",
                operation,
                response.status_code,
                attempt,
                self.max_attempts,
            )
            self._backoff(attempt, response)
        assert last is not None
        raise last

    def list_objects(self, prefix: str) -> list[ObjectRecord]:
        self.require_capabilities({"listFiles"})
        records: list[ObjectRecord] = []
        start_name: str | None = None
        while True:
            body: dict[str, Any] = {
                "bucketId": self.bucket_id,
                "prefix": prefix,
                "maxFileCount": 1000,
            }
            if start_name:
                body["startFileName"] = start_name
            payload = self._api("b2_list_file_names", body, "B2 inventory")
            for value in payload.get("files", []):
                if value.get("action") != "upload":
                    continue
                if int(value.get("contentLength", 0)) <= 0:
                    continue
                info = {
                    str(k): str(v) for k, v in dict(value.get("fileInfo") or {}).items()
                }
                sha1 = str(
                    value.get("contentSha1") or info.get("large_file_sha1") or ""
                )
                records.append(
                    ObjectRecord(
                        key=str(value["fileName"]),
                        size=int(value["contentLength"]),
                        sha1=sha1,
                        file_id=str(value.get("fileId", "")),
                        content_type=str(
                            value.get("contentType", "application/octet-stream")
                        ),
                        metadata=info,
                    )
                )
            start_name = payload.get("nextFileName")
            if not start_name:
                break
        return records

    def find_object(self, key: str) -> ObjectRecord | None:
        self.require_capabilities({"listFiles"})
        payload = self._api(
            "b2_list_file_names",
            {"bucketId": self.bucket_id, "startFileName": key, "maxFileCount": 1},
            "B2 object lookup",
        )
        for value in payload.get("files", []):
            if value.get("action") == "upload" and value.get("fileName") == key:
                if int(value.get("contentLength", 0)) <= 0:
                    return None
                info = {
                    str(k): str(v) for k, v in dict(value.get("fileInfo") or {}).items()
                }
                return ObjectRecord(
                    key=key,
                    size=int(value["contentLength"]),
                    sha1=str(
                        value.get("contentSha1") or info.get("large_file_sha1") or ""
                    ),
                    file_id=str(value.get("fileId", "")),
                    content_type=str(
                        value.get("contentType", "application/octet-stream")
                    ),
                    metadata=info,
                )
        return None

    def _get_upload_endpoint(self) -> dict[str, str]:
        cached = getattr(self._thread, "upload_endpoint", None)
        if cached:
            return cached
        value = self._api(
            "b2_get_upload_url", {"bucketId": self.bucket_id}, "B2 get upload endpoint"
        )
        endpoint = {
            "uploadUrl": str(value["uploadUrl"]),
            "authorizationToken": str(value["authorizationToken"]),
        }
        self._thread.upload_endpoint = endpoint
        return endpoint

    def invalidate_upload_endpoint(self) -> None:
        self._thread.upload_endpoint = None

    @staticmethod
    def hash_file(path: Path) -> tuple[int, str]:
        digest = hashlib.sha1()
        size = 0
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
                size += len(chunk)
                digest.update(chunk)
        return size, digest.hexdigest()

    def upload_file(
        self,
        local_path: Path,
        key: str,
        *,
        content_type: str = "b2/x-auto",
        metadata: dict[str, str] | None = None,
        expected_size: int | None = None,
        expected_sha1: str | None = None,
    ) -> ObjectRecord:
        self.require_capabilities({"writeFiles"})
        actual_size, actual_sha1 = self.hash_file(local_path)
        if expected_size is not None and actual_size != expected_size:
            raise RuntimeError("Local file size changed before B2 upload")
        if expected_sha1 is not None and actual_sha1 != expected_sha1:
            raise RuntimeError("Local file hash changed before B2 upload")
        clean_metadata = {
            str(name).lower().replace("_", "-"): str(value)
            for name, value in (metadata or {}).items()
        }
        last: B2Error | None = None
        for attempt in range(1, self.max_attempts + 1):
            endpoint = self._get_upload_endpoint()
            headers = {
                "Authorization": endpoint["authorizationToken"],
                "X-Bz-File-Name": urllib.parse.quote(key, safe="/"),
                "Content-Type": content_type,
                "Content-Length": str(actual_size),
                "X-Bz-Content-Sha1": actual_sha1,
            }
            for name, value in clean_metadata.items():
                headers[f"X-Bz-Info-{name}"] = urllib.parse.quote(value, safe="")
            try:
                with local_path.open("rb") as handle:
                    response = self.session.request(
                        "POST",
                        endpoint["uploadUrl"],
                        headers=headers,
                        data=handle,
                        timeout=(60, 1800),
                    )
            except requests.RequestException as exc:
                self.invalidate_upload_endpoint()
                last = B2Error("B2 object upload", None, type(exc).__name__, True)
                if attempt == self.max_attempts:
                    raise last from exc
                self.retry_events += 1
                log.warning(
                    "B2 upload endpoint network failure; discarding it before fresh-endpoint retry %s/%s",
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt)
                continue
            if response.ok:
                payload = response.json()
                if not self._valid_upload_ack(
                    payload, key, actual_size, actual_sha1, clean_metadata
                ):
                    self.invalidate_upload_endpoint()
                    raise RuntimeError(
                        "B2 upload acknowledgement did not match the intended object"
                    )
                return ObjectRecord(
                    key=key,
                    size=actual_size,
                    sha1=actual_sha1,
                    file_id=str(payload.get("fileId", "")),
                    content_type=str(payload.get("contentType", content_type)),
                    metadata={
                        str(k): str(v)
                        for k, v in dict(payload.get("fileInfo") or {}).items()
                    },
                )
            retryable = is_retryable_response(response)
            last = B2Error(
                "B2 object upload",
                response.status_code,
                response_code(response),
                retryable,
            )
            self.invalidate_upload_endpoint()
            if not retryable or attempt == self.max_attempts:
                raise last
            self.retry_events += 1
            log.warning(
                "B2 upload endpoint HTTP %s; discarding it before fresh-endpoint retry %s/%s",
                response.status_code,
                attempt,
                self.max_attempts,
            )
            self._backoff(attempt, response)
        assert last is not None
        raise last

    @staticmethod
    def _valid_upload_ack(
        payload: dict[str, Any],
        key: str,
        size: int,
        sha1: str,
        metadata: dict[str, str],
    ) -> bool:
        info = {str(k): str(v) for k, v in dict(payload.get("fileInfo") or {}).items()}
        return (
            payload.get("fileName") == key
            and int(payload.get("contentLength", -1)) == size
            and payload.get("contentSha1") == sha1
            and all(info.get(name) == value for name, value in metadata.items())
        )

    def upload_bytes(
        self,
        payload: bytes,
        key: str,
        *,
        content_type: str,
        metadata: dict[str, str] | None = None,
    ) -> ObjectRecord:
        import tempfile

        with tempfile.TemporaryDirectory(prefix="boink-b2-state-") as directory:
            path = Path(directory) / "payload"
            path.write_bytes(payload)
            return self.upload_file(
                path, key, content_type=content_type, metadata=metadata
            )

    def download_file(self, record: ObjectRecord, ready_path: Path) -> Path:
        self.require_capabilities({"readFiles"})
        expected_sha1 = (
            record.sha1
            if record.sha1 != "none"
            else record.metadata.get("large_file_sha1", "")
        )
        if len(expected_sha1) != 40:
            raise RuntimeError("B2 object lacks a verifiable SHA-1")
        ready_path.parent.mkdir(parents=True, exist_ok=True)
        partial = ready_path.with_name(f".{ready_path.name}.partial")
        last: B2Error | None = None
        for attempt in range(1, self.max_attempts + 1):
            partial.unlink(missing_ok=True)
            auth = self.authorize()
            url = (
                f"{auth['downloadUrl']}/file/{urllib.parse.quote(self.bucket_name, safe='')}"
                f"/{urllib.parse.quote(record.key, safe='/')}"
            )
            try:
                response = self.session.request(
                    "GET",
                    url,
                    headers={"Authorization": auth["authorizationToken"]},
                    stream=True,
                    timeout=(60, 1800),
                )
            except requests.RequestException as exc:
                last = B2Error("B2 object download", None, type(exc).__name__, True)
                if attempt == self.max_attempts:
                    raise last from exc
                self.retry_events += 1
                log.warning(
                    "B2 download network retry %s/%s; partial file discarded",
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt)
                continue
            if not response.ok:
                code = response_code(response)
                retryable = is_retryable_response(response)
                if response.status_code == 401 and code in EXPIRED_TOKEN_CODES:
                    self.authorize(force=True)
                last = B2Error(
                    "B2 object download", response.status_code, code, retryable
                )
                if not retryable or attempt == self.max_attempts:
                    raise last
                self.retry_events += 1
                log.warning(
                    "B2 download HTTP %s retry %s/%s; partial file discarded",
                    response.status_code,
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt, response)
                continue
            digest = hashlib.sha1()
            actual_size = 0
            try:
                with partial.open("wb") as handle:
                    for chunk in response.iter_content(8 * 1024 * 1024):
                        if chunk:
                            handle.write(chunk)
                            digest.update(chunk)
                            actual_size += len(chunk)
            except (requests.RequestException, OSError) as exc:
                partial.unlink(missing_ok=True)
                if attempt == self.max_attempts:
                    raise B2Error(
                        "B2 object download", None, type(exc).__name__, True
                    ) from exc
                self.retry_events += 1
                log.warning(
                    "B2 download stream retry %s/%s; partial file discarded",
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt)
                continue
            if actual_size == record.size and digest.hexdigest() == expected_sha1:
                partial.replace(ready_path)
                return ready_path
            partial.unlink(missing_ok=True)
            last = B2Error(
                "B2 object download verification", None, "integrity_mismatch", True
            )
            if attempt == self.max_attempts:
                raise last
            self.retry_events += 1
            log.warning(
                "B2 download integrity retry %s/%s; partial file discarded",
                attempt,
                self.max_attempts,
            )
            self._backoff(attempt)
        assert last is not None
        raise last

    def download_bytes(self, record: ObjectRecord) -> bytes:
        import tempfile

        with tempfile.TemporaryDirectory(prefix="boink-b2-read-") as directory:
            path = self.download_file(record, Path(directory) / "ready")
            return path.read_bytes()

    def read_json(self, key: str) -> dict[str, Any] | list[Any] | None:
        record = self.find_object(key)
        if record is None:
            return None
        value = json.loads(self.download_bytes(record))
        if not isinstance(value, (dict, list)):
            raise TypeError("B2 JSON state has an invalid root type")
        return value

    def write_json(self, key: str, value: Any, *, purpose: str) -> ObjectRecord:
        payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
        return self.upload_bytes(
            payload,
            key,
            content_type="application/json",
            metadata={
                "boink-purpose": purpose,
                "boink-sha256": hashlib.sha256(payload).hexdigest(),
            },
        )
