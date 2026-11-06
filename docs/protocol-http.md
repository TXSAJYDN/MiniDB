# MiniDB HTTP/1.1 protocol

## Scope

MiniDB implements a deliberately small HTTP/1.1 request/response subset. It is
not a general web server and does not claim RFC-complete HTTP support.

A request must use:

- one request per HTTP message;
- the origin-form request target (a path beginning with `/`);
- exactly **HTTP/1.1**;
- CRLF line endings;
- `Content-Length` for any request body.

The parser rejects HTTP/1.0 and other well-formed versions with 505, malformed
versions with 400, and unsupported transfer framing rather than guessing at a
message boundary.

## Wire limits

| Item | Limit |
|---|---:|
| Request line including CRLF | 4 KiB |
| Request target (path plus optional query) | 4 KiB |
| Header section after the request line | 16 KiB total |
| Header fields | 64 |
| Request body | 1 MiB |
| Per-connection pending/input budget | 1.25 MiB |
| Per-connection output budget | 1.25 MiB |
| Concurrent connections per process | 32 |
| Idle/incomplete-request timeout | 30 seconds |

The request-line limit is enforced while a line is still incomplete, so a peer
cannot grow the buffer to 64 KiB or more merely by withholding CRLF. Header
bytes are accumulated across complete and incomplete lines. One connection may
pipeline multiple requests, but the server handles only one request at a time
and preserves response order.

## Request grammar

At a high level:

~~~text
GET /kv/example HTTP/1.1\r\n
Host: 127.0.0.1\r\n
Connection: keep-alive\r\n
\r\n
~~~

The method must be a valid HTTP token. Recognized methods are GET, POST, HEAD,
PUT, and DELETE. A syntactically valid extension method is preserved and returns
501 because no route accepts it.

### Host

A syntactically valid request must contain one non-empty `Host` field. Supported
forms are DNS-style host names, IPv4 literals, bracketed IPv6 literals, and an
optional decimal port 0..65535. Duplicate Host fields are rejected.

### Other headers

- Field names are case-insensitive and are stored lower-case.
- Leading/trailing optional whitespace around a field value is removed.
- Control characters in field values are rejected.
- Obsolete line folding is rejected with 400.
- Duplicate `Content-Length` is accepted only when every numeric value is the
  same; leading zeroes are allowed.
- Duplicate `Transfer-Encoding` and Host fields are rejected.
- `Connection` fields may be repeated and are combined as a token list.
- Complete header lines are subject to both the byte and field-count limits.

### Framing

`Content-Length` syntax is a non-negative decimal integer. Signs, whitespace,
empty values, and non-digits are invalid. A value above 1 MiB yields 413.

The server does not support:

- `Transfer-Encoding` / chunked bodies: 501;
- `Expect: 100-continue`: 417;
- `Upgrade` / WebSocket: 400.

GET and HEAD requests with a nonzero Content-Length are rejected with 400. An
empty POST body and an empty value are valid.

## Routing

Routing uses the decoded-free path component. Percent escapes are not decoded.
The query string is retained in the parsed request but is not used for routing.

| Method | Path | Behavior |
|---|---|---|
| GET | `/health` | Returns `OK\n` with 200 |
| POST | `/echo` | Returns the exact request body as `application/octet-stream` |
| POST | `/save` | Synchronously persists the current store and returns JSON status |
| GET | `/kv/<key>` | Returns the raw value or 404 |
| POST | `/kv/<key>` | Stores the exact body as the value and returns 200 |
| DELETE | `/kv/<key>` | Deletes a live key or returns 404 |

For `/kv/<key>`:

- the path prefix is exactly four bytes: `/kv/`;
- the key is the remaining raw byte sequence of the path;
- the key must be 1..1024 bytes;
- values are binary-safe and may be empty;
- POST clears any existing TTL for the key;
- HTTP does not expose EXPIRE or TTL operations; use the binary protocol for
  those commands.

A known path with the wrong method returns 405 with an `Allow` header. A known
method on an unknown path returns 404. A valid extension method returns 501.

## Responses

Every response includes `Content-Length` and either `Connection: Keep-Alive` or
`Connection: close`. There is no Date or Server header. HTTP/1.1 connections
persist unless the request contains the `close` connection token or an error
requires connection termination.

Route-level 404/405 responses normally preserve keep-alive. Parse/validation
errors are final: the server writes the error response and shuts down the
connection.

| Status | Meaning in MiniDB |
|---:|---|
| 200 | Successful route |
| 400 | Malformed request, invalid Host, invalid method token, unsupported Upgrade, or bad framing |
| 404 | Unknown path or missing KV key |
| 405 | Known path, unsupported method |
| 408 | Idle/incomplete request timeout |
| 413 | Body or pending input exceeds limit |
| 414 | Request line/target exceeds limit |
| 417 | Expect header present |
| 431 | Header bytes/fields exceed limit |
| 500 | Internal handler or save failure |
| 501 | Valid extension method, Transfer-Encoding, or no handler |
| 505 | Well-formed HTTP version other than 1.1 |

## Examples

Health:

~~~bash
curl -i http://127.0.0.1:8080/health
~~~

Store and fetch a binary-safe value:

~~~bash
curl -i -X POST --data-binary 'value' http://127.0.0.1:8080/kv/key
curl -i http://127.0.0.1:8080/kv/key
~~~

Delete:

~~~bash
curl -i -X DELETE http://127.0.0.1:8080/kv/key
~~~

Explicit save:

~~~bash
curl -i -X POST http://127.0.0.1:8080/save
~~~

The process also saves on graceful SIGINT/SIGTERM shutdown. A successful
`/save` response means the snapshot and parent-directory fsync completed.
