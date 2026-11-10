# RDB v2 format

## Purpose

RDB is MiniDB's full-database checkpoint format. It is written by HTTP
`POST /save`, the KV `SAVE` command, automatic checkpointing before WAL resource
limits are exhausted, and graceful process shutdown. It is not the per-write
durability mechanism; the WAL carries mutations after the checkpoint.

MiniDB enforces single-process ownership with `<rdb_path>.lock`. Do not treat
RDB as an independently writable format and do not bypass the lock.

Version 2 is the only accepted version. Version 1 files are not migrated.

## Byte order and checksum

All fixed-width integers are big-endian. Length prefixes inside records use an
unsigned LEB128-style varint, limited to a valid uint32 encoding of at most five
bytes. CRC32C uses the reflected Castagnoli polynomial and covers every byte
before the final checksum field.

## File layout

~~~text
offset  size       field
0       4          magic = "MRDB"
4       4          version = 2 (u32 BE)
8       4          record_count (u32 BE)
12      variable   record * record_count
end-4   4          crc32c (u32 BE) over bytes [0, end-4)
~~~

The header is 12 bytes. The minimum valid file is 16 bytes (12-byte header plus
4-byte checksum) and represents an empty record set.

## Record layout

~~~text
key_len    : varint
key        : key_len raw bytes
value_len  : varint
value      : value_len raw bytes
expiry_ms  : i64 BE
~~~

- key_len must be 1..1024.
- value_len must be 0..1,048,576.
- A value may be empty.
- expiry_ms is an absolute Unix epoch millisecond timestamp.
- Expiry 0 means no expiry.
- Negative expiry values are invalid.
- Keys must be unique in one snapshot.
- Expired records are omitted from a new save, and an expired record encountered
  during recovery is discarded by restore_entry().
- The key and value payloads are arbitrary octets and are not interpreted as
  text.

## Varint encoding

encode_varint() emits seven payload bits per byte, least-significant group
first. The high bit indicates continuation. Valid uint32 values use at most five
bytes.

The decoder rejects:

- a continuation bit on the fifth byte;
- a fifth-byte payload greater than 0x0f;
- an encoding that would require more than five bytes;
- a truncated encoding.

The boundary cases are:

~~~text
0                 00
127               7f
128               80 01
0xffffffff        ff ff ff ff 0f
overflow sample   ff ff ff ff 10
~~~

The overflow sample has a correct checksum when used in a test file; rejection
must come from varint validation, not a checksum mismatch.

## Limits

| Limit | Value |
|---|---:|
| Maximum file size | 64 MiB |
| Maximum records loaded | 100,000 |
| Maximum key size | 1 KiB |
| Maximum value size | 1 MiB |
| Maximum recovered key+value bytes | 256 MiB |
| Header | 12 bytes |
| Checksum | 4 bytes |

The recovered key/value budget is intentionally larger than the maximum file
size to cover records whose stored payload is expanded into in-memory strings
and map overhead, while still bounding allocation.

## Save algorithm

1. Lock the per-manager save mutex.
2. Export live entries and their absolute expiry times under the store lock.
3. Reject empty or oversized keys, oversized values, negative expiry, too many
   records, or a serialized image above 64 MiB.
4. Build the header and every record in memory.
5. Compute CRC32C over the unfinished image and append the checksum.
6. Create a unique temporary path with O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC
   and mode 0600.
7. Write all bytes with short-write and EINTR handling.
8. fsync the temporary file.
9. Close it, checking the close result.
10. Rename it atomically over the destination.
11. Open and fsync the parent directory.

Failure before rename leaves the old destination in place where possible and
removes the temporary file. Failure to fsync the parent after a successful
rename is reported as RdbSaveResult::DurabilityUnconfirmed: the new snapshot may
be visible, but crash durability is not confirmed. The process exit path treats
that result as failure.

## Load algorithm

1. At the RdbManager component level, treat a missing destination as an empty
   database and return 0. Full startup recovery may still reject a missing RDB
   when a manifest or WAL proves that checkpoint state should exist; see
   [Recovery model](recovery.md).
2. Require a regular file and a size of 16..64 MiB.
3. Read the complete file and verify that no trailing unconsumed bytes remain.
4. Verify CRC32C and the four-byte magic.
5. Verify version is exactly 2.
6. Reject record_count above 100,000.
7. Decode records one by one into a fresh temporary KvStore:
   - decode key length and preserve exact record boundaries;
   - reject empty, oversized, truncated, duplicate, or negative-expiry entries;
   - accumulate and enforce the recovery byte budget;
   - restore with the stored absolute expiry.
8. Require the parsed offset to equal the start of the checksum field.
9. Only after complete success, swap the temporary store into the caller.

Therefore, a malformed, truncated, oversized, checksum-invalid, duplicate-key,
or varint-overflow image returns -1 and does not modify the existing store. The
missing-file case is the only successful "nothing to load" outcome.

## Example

An empty database is:

~~~text
4d 52 44 42   "MRDB"
00 00 00 02   version 2
00 00 00 00   zero records
XX XX XX XX   CRC32C of the preceding 12 bytes
~~~

A single record with key "k", value "v", and no expiry has the body:

~~~text
01 6b          key_len=1, key="k"
01 76          value_len=1, value="v"
00 00 00 00 00 00 00 00   expiry_ms=0
~~~

The checksum is then computed over the header plus that record body.

## Failure diagnostics

The public load interface returns a record count or -1. RdbManager does not
partially publish a failed load. Callers such as HttpServer::load_rdb() and
KvServer::load_rdb() convert -1 into a startup failure with a nonzero process
exit code.

Typical causes of -1:

- file shorter than 16 bytes;
- magic or version mismatch;
- checksum mismatch;
- declared count over the limit;
- truncated key/value/expiry;
- payload length beyond its limit;
- duplicate key;
- varint overflow;
- trailing bytes after the declared records;
- invalid path type or I/O failure.

## Operational rules

- Use a filesystem that supports atomic rename and directory fsync.
- Ensure the process can create and fsync both the RDB file and its parent
  directory.
- Do not run two MiniDB processes against one RDB path. The process takes an
  advisory exclusive `flock` on `<rdb_path>.lock` for its lifetime.
- A current-state backup requires the RDB, `<rdb_path>.manifest`, and the
  `<rdb_path>.wal/` directory, or a stopped process and a consistent copy of the
  whole set. Copying only the RDB can omit mutations after the checkpoint.
- A checksum failure is not repaired automatically. Preserve the complete RDB,
  manifest, and WAL set for diagnosis and restore a known-good backup.
