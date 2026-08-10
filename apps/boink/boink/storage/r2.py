from __future__ import annotations

import hashlib
import json
import logging
import mimetypes
import random
import time
from collections.abc import Callable, Iterable
from pathlib import Path
from typing import Any

import boto3
from boto3.s3.transfer import TransferConfig
from botocore.config import Config
from botocore.exceptions import BotoCoreError, ClientError

from ..config import BoinkConfig, required_env

RETRYABLE_STATUS = {408, 429, 500, 502, 503, 504}
RETRYABLE_CODES = {
    "RequestTimeout",
    "SlowDown",
    "InternalError",
    "ServiceUnavailable",
    "Throttling",
}
NOT_FOUND_CODES = {"NoSuchKey", "404", "NotFound"}
log = logging.getLogger("boink.r2")


class R2Error(RuntimeError):
    pass


def retryable_exception(exc: Exception) -> bool:
    if isinstance(exc, BotoCoreError) and not isinstance(exc, ClientError):
        return True
    if isinstance(exc, (OSError, TimeoutError)):
        return True
    if isinstance(exc, ClientError):
        response = exc.response
        status = int(response.get("ResponseMetadata", {}).get("HTTPStatusCode", 0))
        code = str(response.get("Error", {}).get("Code", ""))
        return status in RETRYABLE_STATUS or code in RETRYABLE_CODES
    return False


class R2Transport:
    """The single authoritative R2 client for Boink."""

    def __init__(
        self,
        *,
        client: Any,
        bucket_name: str,
        max_attempts: int = 6,
        sleep: Callable[[float], None] = time.sleep,
        jitter: Callable[[], float] = random.random,
    ):
        self.client = client
        self.bucket_name = bucket_name
        self.max_attempts = max_attempts
        self._sleep = sleep
        self._jitter = jitter
        self.retry_events = 0
        self.transfer_config = TransferConfig(
            multipart_threshold=16 * 1024 * 1024,
            multipart_chunksize=8 * 1024 * 1024,
            max_concurrency=4,
            use_threads=True,
        )

    @classmethod
    def from_env(cls, config: BoinkConfig) -> R2Transport:
        account_id = required_env("R2_ACCOUNT_ID")
        client = boto3.client(
            "s3",
            endpoint_url=f"https://{account_id}.r2.cloudflarestorage.com",
            aws_access_key_id=required_env("R2_ACCESS_KEY_ID"),
            aws_secret_access_key=required_env("R2_SECRET_ACCESS_KEY"),
            region_name="auto",
            config=Config(
                signature_version="s3v4",
                retries={"max_attempts": 10, "mode": "adaptive"},
                connect_timeout=30,
                read_timeout=180,
                tcp_keepalive=True,
            ),
        )
        return cls(
            client=client,
            bucket_name=required_env("R2_BUCKET_NAME"),
            max_attempts=config.max_attempts,
        )

    def _backoff(self, attempt: int) -> None:
        self._sleep(min(60.0, 2.0 ** (attempt - 1)) + self._jitter())

    def _call(self, operation: str, function: Callable[..., Any], **kwargs: Any) -> Any:
        last: Exception | None = None
        for attempt in range(1, self.max_attempts + 1):
            try:
                return function(**kwargs)
            except Exception as exc:
                last = exc
                if not retryable_exception(exc) or attempt == self.max_attempts:
                    raise R2Error(
                        f"{operation} failed after {attempt} attempt(s)"
                    ) from exc
                self.retry_events += 1
                log.warning(
                    "%s transient retry %s/%s",
                    operation,
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt)
        assert last is not None
        raise R2Error(f"{operation} failed") from last

    def head(self, key: str) -> dict[str, Any] | None:
        try:
            return self._call(
                "R2 HEAD",
                self.client.head_object,
                Bucket=self.bucket_name,
                Key=key,
            )
        except R2Error as wrapped:
            cause = wrapped.__cause__
            if isinstance(cause, ClientError):
                code = str(cause.response.get("Error", {}).get("Code", ""))
                if code in NOT_FOUND_CODES:
                    return None
            raise

    def verify_object(
        self,
        key: str,
        *,
        expected_size: int,
        expected_sha1: str = "",
        generation: str = "",
    ) -> bool:
        value = self.head(key)
        if value is None or int(value.get("ContentLength", -1)) != expected_size:
            return False
        metadata = {
            str(k).lower(): str(v) for k, v in dict(value.get("Metadata") or {}).items()
        }
        if expected_sha1 and metadata.get("boink-sha1") != expected_sha1:
            return False
        return not generation or metadata.get("boink-generation") == generation

    def upload_file(
        self,
        local_path: Path,
        key: str,
        *,
        expected_size: int,
        expected_sha1: str,
        generation: str,
        source_identity: str,
        content_type: str = "",
    ) -> None:
        actual_size = local_path.stat().st_size
        if actual_size != expected_size:
            raise RuntimeError("Refusing R2 upload because staged size is wrong")
        digest = hashlib.sha1()
        with local_path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(8 * 1024 * 1024), b""):
                digest.update(chunk)
        if digest.hexdigest() != expected_sha1:
            raise RuntimeError("Refusing R2 upload because staged hash is wrong")
        extra = {
            "ContentType": content_type
            or mimetypes.guess_type(local_path.name)[0]
            or "application/octet-stream",
            "CacheControl": "private, max-age=86400",
            "Metadata": {
                "boink-sha1": expected_sha1,
                "boink-generation": generation,
                "boink-source-id": source_identity,
            },
        }
        self._call(
            "R2 upload",
            self.client.upload_file,
            Filename=str(local_path),
            Bucket=self.bucket_name,
            Key=key,
            ExtraArgs=extra,
            Config=self.transfer_config,
        )
        if not self.verify_object(
            key,
            expected_size=expected_size,
            expected_sha1=expected_sha1,
            generation=generation,
        ):
            raise RuntimeError("R2 acknowledgement/HEAD verification failed")

    def get_bytes(self, key: str) -> bytes | None:
        try:
            response = self._call(
                "R2 GET", self.client.get_object, Bucket=self.bucket_name, Key=key
            )
        except R2Error as wrapped:
            cause = wrapped.__cause__
            if isinstance(cause, ClientError):
                code = str(cause.response.get("Error", {}).get("Code", ""))
                if code in NOT_FOUND_CODES:
                    return None
            raise
        body = response["Body"]
        try:
            return body.read()
        finally:
            body.close()

    def download_file(
        self,
        key: str,
        ready_path: Path,
        *,
        expected_size: int | None = None,
        expected_sha1: str = "",
    ) -> Path:
        ready_path.parent.mkdir(parents=True, exist_ok=True)
        partial = ready_path.with_name(f".{ready_path.name}.partial")
        for attempt in range(1, self.max_attempts + 1):
            partial.unlink(missing_ok=True)
            try:
                response = self.client.get_object(Bucket=self.bucket_name, Key=key)
                digest = hashlib.sha1()
                size = 0
                with partial.open("wb") as output:
                    while True:
                        chunk = response["Body"].read(8 * 1024 * 1024)
                        if not chunk:
                            break
                        output.write(chunk)
                        digest.update(chunk)
                        size += len(chunk)
                response["Body"].close()
                if expected_size is not None and size != expected_size:
                    raise OSError("R2 download size mismatch")
                if expected_sha1 and digest.hexdigest() != expected_sha1:
                    raise OSError("R2 download hash mismatch")
                partial.replace(ready_path)
                return ready_path
            except Exception as exc:
                partial.unlink(missing_ok=True)
                if (
                    not retryable_exception(exc) and not isinstance(exc, OSError)
                ) or attempt == self.max_attempts:
                    raise R2Error("R2 download failed or was incomplete") from exc
                self.retry_events += 1
                log.warning(
                    "R2 download retry %s/%s; partial file discarded",
                    attempt,
                    self.max_attempts,
                )
                self._backoff(attempt)
        raise R2Error("R2 download exhausted retries")

    def put_bytes(
        self,
        key: str,
        payload: bytes,
        *,
        content_type: str,
        cache_control: str = "no-store",
        metadata: dict[str, str] | None = None,
        verify_body: bool = True,
    ) -> dict[str, Any]:
        sha256 = hashlib.sha256(payload).hexdigest()
        clean_metadata = {str(k).lower(): str(v) for k, v in (metadata or {}).items()}
        clean_metadata["boink-sha256"] = sha256
        self._call(
            "R2 atomic object publication",
            self.client.put_object,
            Bucket=self.bucket_name,
            Key=key,
            Body=payload,
            ContentType=content_type,
            CacheControl=cache_control,
            Metadata=clean_metadata,
        )
        head = self.head(key)
        if (
            head is None
            or int(head.get("ContentLength", -1)) != len(payload)
            or dict(head.get("Metadata") or {}).get("boink-sha256") != sha256
        ):
            raise RuntimeError("R2 published-object metadata verification failed")
        if verify_body and self.get_bytes(key) != payload:
            raise RuntimeError("R2 published-object body verification failed")
        return head

    def put_json(
        self, key: str, value: Any, *, generation: str = "", verify_body: bool = True
    ) -> bytes:
        payload = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
        metadata = {"boink-generation": generation} if generation else {}
        self.put_bytes(
            key,
            payload,
            content_type="application/json; charset=utf-8",
            metadata=metadata,
            verify_body=verify_body,
        )
        return payload

    def list_keys(self, prefix: str) -> list[dict[str, Any]]:
        values: list[dict[str, Any]] = []
        paginator = self.client.get_paginator("list_objects_v2")
        try:
            pages = paginator.paginate(
                Bucket=self.bucket_name,
                Prefix=prefix,
                PaginationConfig={"PageSize": 1000},
            )
            for page in pages:
                for value in page.get("Contents", []):
                    values.append(
                        {
                            "key": str(value["Key"]),
                            "size": int(value["Size"]),
                            "etag": str(value.get("ETag", "")).strip('"'),
                            "last_modified": value.get("LastModified"),
                        }
                    )
        except Exception as exc:
            raise R2Error("R2 paginated listing failed") from exc
        return values

    def delete_keys(
        self, keys: Iterable[str], *, protected: set[str] | None = None
    ) -> list[str]:
        protected = protected or set()
        wanted = sorted({str(key) for key in keys if key and key not in protected})
        failed: list[str] = []
        for offset in range(0, len(wanted), 1000):
            batch = wanted[offset : offset + 1000]
            try:
                response = self._call(
                    "R2 batch delete",
                    self.client.delete_objects,
                    Bucket=self.bucket_name,
                    Delete={"Objects": [{"Key": key} for key in batch], "Quiet": False},
                )
            except R2Error:
                failed.extend(batch)
                continue
            error_keys = {
                str(value.get("Key", "")) for value in response.get("Errors", [])
            }
            failed.extend(key for key in batch if key in error_keys)
        return failed
