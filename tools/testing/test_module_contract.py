"""Validate the request example in ADR 0001 without launching a module."""

import json
import re
import unittest


MAX_FRAME_BYTES = 4096
ID = re.compile(r"[a-zA-Z0-9._-]{1,64}")
FIELDS = {"v", "id", "method", "target"}
METHODS = {"media.read", "media.play", "capture.read"}


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate object member")
        result[key] = value
    return result


def parse_request(frame):
    if len(frame) > MAX_FRAME_BYTES:
        raise ValueError("Frame exceeds byte limit")
    try:
        request = json.loads(frame.decode("utf-8"), object_pairs_hook=unique_object)
    except RecursionError as error:
        raise ValueError("Frame nesting exceeds parser limits") from error
    if not isinstance(request, dict) or request.keys() != FIELDS:
        raise ValueError("Unexpected request fields")
    if type(request["v"]) is not int or request["v"] != 1:
        raise ValueError("Unsupported version")
    for field in ("id", "target"):
        if not isinstance(request[field], str) or not ID.fullmatch(request[field]):
            raise ValueError("Invalid request or resource identifier")
    if not isinstance(request["method"], str) or request["method"] not in METHODS:
        raise ValueError("Unsupported method")
    return request


class ModuleContractTest(unittest.TestCase):
    sample = b'{"v":1,"id":"r-12","method":"media.play","target":"session-7"}'

    def test_documented_shape(self):
        self.assertEqual(parse_request(self.sample)["target"], "session-7")

    def test_unknown_and_missing_fields(self):
        for frame in (
            self.sample[:-1] + b',"granted":true}',
            b'{"v":1,"id":"r","method":"media.play"}',
        ):
            with self.subTest(frame=frame), self.assertRaises(ValueError):
                parse_request(frame)

    def test_oversized_frame_and_identifier(self):
        with self.assertRaises(ValueError):
            parse_request(self.sample.ljust(MAX_FRAME_BYTES + 1))
        with self.assertRaises(ValueError):
            parse_request(self.sample.replace(b"r-12", b"r" * 65))

    def test_version_and_method_restrictions(self):
        for frame in (
            self.sample.replace(b'"v":1', b'"v":true'),
            self.sample.replace(b'"v":1', b'"v":2'),
            self.sample.replace(b'"media.play"', b'["media.play"]'),
            self.sample.replace(b"media.play", b"capture.delete"),
        ):
            with self.subTest(frame=frame), self.assertRaises(ValueError):
                parse_request(frame)

    def test_duplicate_and_malformed_members(self):
        for frame in (
            self.sample[:-1] + b',"target":"other"}',
            self.sample.replace(b"session-7", b"../example"),
            self.sample.replace(b"session-7", b"media\xff"),
        ):
            with self.subTest(frame=frame), self.assertRaises(ValueError):
                parse_request(frame)


if __name__ == "__main__":
    unittest.main()
