# MiniDB binary KV protocol

## Transport

The KV server accepts TCP connections, normally on port 6379. The protocol is
binary-safe: keys and values may contain NUL bytes and arbitrary octets. They
are not interpreted as UTF-8 or C strings.

All integers are unsigned big-endian unless explicitly stated otherwise. A
connection is persistent and may carry multiple commands. The server processes
one command at a time per connection and preserves response order.

## Frame format

~~~text
frame_len : u32 BE
command   : u8
argc      : u8
repeat argc times:
    arg_len : u32 BE
    arg     : arg_len raw bytes
~~~

`frame_len` covers `command`, `argc`, and every argument length/data pair. It
does not include the four-byte `frame_len` prefix itself. The declared frame must
be consumed exactly; trailing bytes inside a frame and truncated arguments are
malformed.

## Limits

| Item | Limit |
|---|---:|
| Single argument | 1 MiB |
| Frame payload | 1 MiB + 64 KiB |
| Arguments per command | 255 (u8 range) |
| Key length for key commands | 1..1024 bytes |
| Connection pending/input budget | 1.25 MiB |
| Concurrent connections per process | 32 |

Empty values are valid. Empty keys are rejected by key commands. The maximum
frame budget includes framing overhead and allows a 1 KiB key plus a 1 MiB
value.

## Request commands

| Name | Code | Arguments | Success response |
|---|---:|---|---|
| PING | 1 | 0 | OK with one argument `PONG` |
| SET | 2 | key, value | OK with no arguments |
| GET | 3 | key | VALUE with one argument, or NOTFOUND |
| DEL | 4 | key | INTEGER `1` if removed, else `0` |
| EXPIRE | 5 | key, ttl_ms | OK with no arguments |
| TTL | 6 | key | INTEGER, semantics below |
| SAVE | 7 | 0 | OK if snapshot durable, else ERROR |

### SET

Stores or replaces the value and clears any existing TTL. The value may be
empty. A successful response is:

~~~text
OK argc=0
~~~

### GET

Returns:

- VALUE with one raw-byte argument when a live key exists;
- NOTFOUND when the key is missing or expired.

### DEL

Returns INTEGER `1` only when a live key was removed. Missing or expired keys
return INTEGER `0`.

### EXPIRE

The second argument is a base-10 signed 64-bit integer in milliseconds parsed
with `std::from_chars`. Signs are accepted. A non-positive TTL removes a live
key. The current protocol returns OK whether or not the key existed, matching
the store operation's application semantics but not exposing an existence bit.

TTL deadlines use the process monotonic clock. Wall-clock jumps do not change a
live deadline.

### TTL

Returns an INTEGER argument:

| Value | Meaning |
|---:|---|
| `-1` | key exists and has no expiry |
| `-2` | key is missing or expired |
| non-negative integer | remaining lifetime in milliseconds, rounded down |

### SAVE

Runs the same synchronous snapshot path as the HTTP `/save` route. OK means the
temporary file was written/fsynced, atomically renamed, and the parent directory
was fsynced. A rename succeeded followed by directory-fsync failure is reported
as ERROR with `RDB durability unconfirmed`; the new path may already be visible.

## Response commands

| Name | Code | Meaning |
|---|---:|---|
| ERROR | 100 | One argument containing a human-readable error |
| OK | 101 | Zero or more arguments, command-specific |
| VALUE | 102 | One raw value argument |
| NOTFOUND | 103 | No arguments |
| INTEGER | 104 | One decimal ASCII integer argument |

Responses use exactly the same frame layout as requests.

## Error and connection behavior

- Invalid argument count, invalid key size/length, invalid TTL text, or SAVE
  failure returns ERROR but leaves the connection usable.
- An unknown command code returns ERROR and closes the connection.
- A malformed frame, oversized declared frame, truncated final frame that
  exceeds the pending budget, or response that cannot be represented within the
  wire limit returns ERROR where possible and closes the connection.
- The server does not silently resynchronize after framing corruption.
- There is no KV idle timeout and no authentication/TLS layer.

## Examples

PING request:

~~~text
00 00 00 02 01 00
~~~

PING response:

~~~text
00 00 00 0a 65 01 00 00 00 04 50 4f 4e 47
~~~

SET key `k` value `v` request:

~~~text
00 00 00 0c 02 02
00 00 00 01 6b
00 00 00 01 76
~~~

SET response:

~~~text
00 00 00 02 65 00
~~~

GET key `k` request:

~~~text
00 00 00 08 03 01 00 00 00 01 6b
~~~

GET existing key response:

~~~text
00 00 00 0a 66 01 00 00 00 01 76
~~~

GET missing key response:

~~~text
00 00 00 02 67 00
~~~

The Python client `test_kv_client.py` is a runnable smoke example:

~~~bash
python3 test_kv_client.py 6379
~~~
