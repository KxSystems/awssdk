# Aws-sdk References

These are the API specifications for the aws-sdk module.

**Threads:** call this module only from q's main thread. Do not call its functions inside `peach`
or on q's secondary threads, and do not create handles there, since a handle that is released on
a secondary thread runs its cleanup there. The module does its own work on background threads, but
it relies on every call coming from the main thread.

## initialize

Initializes the SDK. Must be called before any other function in this module.

**Parameters:**

|Name|Type|Description|
|---|---|---|
|options|dict or `::`|Options to set the AWS SDK, or `::` to accept every default|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|loglevel|string\|symbol|`"INFO"`|AWS SDK log level. One of `OFF`, `FATAL`, `ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE`, matched case-insensitively|
|logPrefix|string|SDK default|Prefix for the SDK's own log files, e.g. `"/tmp/sdk_logs/aws_"`. Ignored when `loglevel` is `OFF`. If it contains a `/` or `\`, that directory is created if missing; failing to create it is an error|

The default `logPrefix` writes files named `aws_sdk_<date>-<hour>.log` in q's current working directory.

**Returns:** `1b` if this call initialized the SDK, `0b` if it was already initialized. A second call makes no changes — including to the log level — so call [`shutDown`](#shutdown) first if you need to change it.

**Errors**

|Error|Cause|
|---|---|
|`'type`|`options` is neither a dict nor `::`, or `logPrefix` is present but not a string|
|`'loglevel`|`loglevel` is present but is not a string, or not one of the values above|
|`'logPrefix`|`logPrefix`'s directory could not be created|

The module registers an exit handler the first time it initializes, so the SDK is shut down on process exit even if [`shutDown`](#shutdown) is never called.

**Example:**

```q
sdk.initialize[::] // defaults: loglevel "INFO", logPrefix in the current directory
1b
sdk.initialize[::] // already initialized, no change
0b
sdk.shutDown[::]
1b
sdk.initialize[([loglevel: "DEBUG"; logPrefix: "/tmp/sdk_logs/aws_"])]
1b
sdk.initialize[([loglevel: "asdf"])]
'loglevel
```

## shutDown

Shuts down the SDK. Call it before exiting the session.

**Returns:** `1b` if this call shut down an initialized SDK, `0b` if there was nothing to shut down. Signals `'busy` if SDK threads were still busy, as described below. Any [`createClient`](#createclient) or [`createTransferManager`](#createtransfermanager) handle still held becomes inert — see [`createClient`](#createclient)'s **Returns**.

`shutDown` must not run while another call into this module is in progress. Every function here runs on q's main thread, so that holds for any q code; it matters only if the module is called from other threads.

Transfers started by [`uploadAsync`](#uploadasync) or [`downloadAsync`](#downloadasync) and still running are canceled, and `shutDown` waits for them to stop before returning. Each one's callback still receives a final report, normally `` `CANCELED ``, once q's event loop next runs: a download through a temporary file leaves no file behind, and a multipart upload is aborted.

The SDK's own threads can still be finishing work for a few moments after a transfer ends. `shutDown` waits up to 10 seconds for them before shutting the SDK down. If they are still busy after that, it signals `'busy` and leaves the SDK initialized, since shutting it down under them is unsafe; every handle is already invalidated, and calling `shutDown` again completes it once they are done. In the unlikely event that the module could not keep track of an SDK object, for example because memory ran out, `shutDown` signals `'busy` from then on, and the SDK is left running until the process exits.

**Example:**

```q
sdk.shutDown[::]
1b
sdk.shutDown[::]
0b
```

## getCredentials

Fetches AWS credentials using the official AWS authentication chain. Requires [`initialize`](#initialize) to have been called.

**Returns:** a dictionary of five keys:

|Key|Type|Description|
|---|---|---|
|accessKey|string|AWS access key ID|
|secretKey|string|AWS secret access key|
|sessionToken|string|Session token. Empty for long-lived credentials|
|accountId|string|AWS account ID. Empty if the provider does not supply one|
|expiration|long|Expiry as milliseconds since the Unix epoch|

A chain that resolves nothing is not an error: the call still succeeds and returns the same five keys with every string empty, so check the result rather than relying on a signal.

Credentials are not cached, so every call resolves the chain again and returns fresh credentials. Hold on to the result rather than calling this in a tight loop.

**Errors**

|Error|Cause|
|---|---|
|`'uninitialized`|[`initialize`](#initialize) has not been called|

**Example:**

```q
sdk.initialize[::]
1b
credentials: sdk.getCredentials[::]
key credentials
`accessKey`secretKey`sessionToken`accountId`expiration
```

## createClient

Creates an S3 client, used by the other S3 functions. Requires [`initialize`](#initialize) to
have been called.

Credentials are resolved through the same default AWS chain as
[`getCredentials`](#getcredentials), unless `noSignRequest`, `credentials` or `profile` is set.

**Parameters:**

|Name|Type|Description|
|---|---|---|
|options|dict|Options for the client, or `::` for the defaults|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|region|string\|symbol|`"aws-global"`|AWS region to use|
|endpointUrl|string|none|Override the endpoint, e.g. for a MinIO or other S3-compatible server|
|virtualAddressing|boolean\|long|`1b`|Use virtual-hosted-style addressing. Set to `0b` for path-style, which S3-compatible servers usually need|
|noSignRequest|boolean\|long|`0b`|Do not use credentials or sign requests, for reading public buckets. The equivalent of the AWS CLI's `--no-sign-request`. Takes precedence: `credentials` and `profile` are ignored when it is set|
|credentials|string|none|Path to an AWS credentials file, e.g. a mounted secret. Read once, when the client is created|
|profile|string|`"default"`|Profile to read from `credentials`. Requires `credentials`|
|caFile|string|SDK default|Path to a CA certificate bundle|
|caPath|string|SDK default|Directory of CA certificates|
|verifySsl|boolean\|long|`1b`|Verify the server's TLS certificate|
|connectTimeout|long|`5000`|Connection timeout in milliseconds. At least 1, up to the platform's `long` maximum|
|requestTimeout|long|`5000`|Inactivity timeout in milliseconds, **not** a cap on total request duration. See the note below|
|maxConnections|long|`25`|Maximum concurrent HTTP connections, 1 to 4294967295|

An unrecognised option name is an error rather than being ignored, so a mis-spelling is reported
instead of silently taking the default.

`requestTimeout` does not limit how long a request may take. It maps to the SDK's
`requestTimeoutMs`, which under libcurl is the *low-speed time*: a transfer is aborted only once it
has stayed below roughly 1 byte/second for that long. A large download that keeps making progress
is not affected by the 5000ms default, however long it runs. Two consequences worth knowing:

- libcurl rounds the value down to whole seconds, except that a value between 1 and 999 becomes one
  second, so sub-second precision is not meaningful.
- `0` disables the low-speed check under libcurl. On the Windows HTTP clients, where this value is a
  socket read timeout instead, `0` is documented as unspecified behaviour.

The SDK's whole-request timeout is a different field (`httpRequestTimeoutMs`, `CURLOPT_TIMEOUT_MS`),
which this module leaves at the SDK default of no limit and does not currently expose.

A count or duration too large for the field the SDK stores it in is rejected rather than wrapped,
so an out-of-range value never silently becomes a small or zero one. The timeout ceiling is the
platform's `long` maximum, which is smaller on Windows than on Linux and macOS.

`connectTimeout`, `requestTimeout` and `maxConnections` default to the same values the `awss3kdb`
module used, which for the two timeouts differs from the AWS SDK's own defaults of 1000ms and no
limit. TLS and connection options apply whether or not the request is signed.

Credentials given through `credentials` are read once, when the client is created, so later edits
to that file do not reach an existing client. That suits fixed keys; rotating or expiring temporary
credentials would need a new client. `profile` without `credentials` is an error rather than being
ignored — to pick a profile from the standard locations, set `AWS_PROFILE` and let the default
chain resolve it. The one exception is `noSignRequest`, which discards credential selection
entirely: with it set, `credentials` and `profile` are ignored rather than rejected.

When `credentials` is given without `profile`, the `default` profile is used; if the file has no
`default` profile but holds exactly one profile, that one is used. A file with several profiles and
no `default` is an error rather than a guess. A `credentials` path that cannot be read or parsed is
an error, rather than falling back to the default credential chain.

**Returns:** a foreign object holding the client. It is garbage collected when its refcount drops
to zero, and is invalidated by [`shutDown`](#shutdown). A handle that outlives a `shutDown` becomes
inert: it can be held and dropped safely, but it never refers to a client created afterwards.


**Example:**

```q
client: sdk.createClient[::]  // defaults

client: sdk.createClient[([region: "eu-west-1"])]

// an S3-compatible server, which needs path-style addressing
client: sdk.createClient[([endpointUrl: "http://localhost:9000"; virtualAddressing: 0b])]

// a public bucket, no credentials
client: sdk.createClient[([region: "eu-west-1"; noSignRequest: 1b])]

// credentials from a mounted secret, selecting a named profile
client: sdk.createClient[([credentials: "/etc/secret/credentials"; profile: "readonly"])]

// a long-running download: no request timeout, more connections
client: sdk.createClient[([region: "eu-west-1"; requestTimeout: 0; maxConnections: 100])]
```

## listObjects

Lists the objects in a bucket. Requires [`initialize`](#initialize) and a client from
[`createClient`](#createclient).

Every page is fetched, so the result is the complete listing however large it is.

**Parameters:**

|Name|Type|Description|
|---|---|---|
|client|foreign|A client from [`createClient`](#createclient)|
|bucket|string\|symbol|Bucket to list|
|options|dict|Options, or `::` for none|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|prefix|string\|symbol|none|List only keys beginning with this prefix|
|startAfter|string\|symbol|none|List only keys ordered after this one, exclusive|

**Returns:** a keyed table of `objectKey` to `size`, where `objectKey` is the object key as a
string and `size` its size in bytes. An empty bucket gives a keyed table with no rows.

The column is `objectKey`, not `key`, because `key` is a reserved word in q: a column of that name
cannot appear in a table literal, and `where key = ...` silently matches nothing instead of
erroring.

Errors from S3 itself carry the service's own text, for example
`` `NoSuchBucket: The specified bucket does not exist ``, rather than the short tokens used to
report a bad argument or option.

**Example:**

```q
client: sdk.createClient[([region: "eu-west-1"])]

listing: sdk.listObjects[client; "my-bucket"; ::]
listing: sdk.listObjects[client; "my-bucket"; ([prefix: "data/2026/"])]

// resume after a key already seen
listing: sdk.listObjects[client; "my-bucket";
    ([prefix: "data/2026/"; startAfter: "data/2026/part-0.parquet"])]

// the size of one object, indexed by its key
listing["data/2026/part-0.parquet"]`size

// as a plain table
0!listing
```

## listObjectsMetadata

Lists the objects in a bucket with the metadata the listing carries, rather than size alone. Takes
the same arguments and options as [`listObjects`](#listobjects), and costs the same — the extra
columns come from the same request, not from a call per object.

**Parameters** and **Options** are exactly those of [`listObjects`](#listobjects): `prefix` and
`startAfter`.

**Returns:** a keyed table of `objectKey` to:

|Column|Type|Description|
|---|---|---|
|size|long|Size in bytes|
|lastModified|timestamp|When the object was last written. Null if the service did not report it|
|eTag|string|The object's ETag, with S3's surrounding double quotes removed. Not necessarily an MD5 digest — multipart uploads and some encryption modes produce values that cannot be compared against one computed locally|
|storageClass|symbol|`` `STANDARD ``, `` `GLACIER ``, and so on|
|ownerId|string|Owner's canonical ID|

The key column is the same as [`listObjects`](#listobjects)', so this is a superset: code written
as ``listing[objectKey]`size`` works against either function.

`ownerId` needs no extra request, only a larger response on the same one. Some S3-compatible
servers, MinIO among them, leave it empty. The owner display name is not exposed: AWS S3 generally
no longer returns one.

Metadata that the listing API cannot provide — content type, content encoding, version id, and
user-defined `x-amz-meta-*` pairs — is not available here. Those require a per-object request and
are not currently exposed.

**Example:**

```q
client: sdk.createClient[([region: "eu-west-1"])]

listing: sdk.listObjectsMetadata[client; "my-bucket"; ([prefix: "data/2026/"])]

// objects written in the last day
select from 0!listing where lastModified > .z.p - 1D

// total bytes under the prefix
exec sum size from listing

// anything not on standard storage
select from 0!listing where not storageClass = `STANDARD
```

## putObject

Uploads a local file to S3 with a synchronous, single-request `PutObject`. Requires
[`initialize`](#initialize) and a client from [`createClient`](#createclient).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|client|foreign|A client from [`createClient`](#createclient)|
|bucket|string\|symbol|Destination bucket|
|key|string\|symbol|Destination object key|
|filename|string\|symbol|Local file to upload|
|options|dict|Reserved for future options; pass `::` or an empty dictionary|

**Returns:** a long containing the number of bytes uploaded.

The input is opened in binary mode and its size is sent as the request content length. A
`filename` that is not a regular file, such as a directory, or that cannot be opened is rejected
before an S3 request is made. The file must not change while it is being uploaded.

This function performs one `PutObject` request. Amazon S3 accepts at most 5 GB in a single
`PutObject`; a larger file is rejected with `EntityTooLarge` before any of it is sent. Larger files
need a multipart upload: see [`upload`](#upload). S3-compatible servers set their own limits.

**Example:**

```q
bytes: sdk.putObject[client; "my-bucket"; "data/input.csv"; "input.csv"; ::]
```

## getObject

Downloads an S3 object to a local file with a synchronous `GetObject`. Requires
[`initialize`](#initialize) and a client from [`createClient`](#createclient).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|client|foreign|A client from [`createClient`](#createclient)|
|bucket|string\|symbol|Source bucket|
|key|string\|symbol|Source object key|
|filename|string\|symbol|Local destination file|
|options|dict|Options, or `::` for the defaults|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|inPlace|boolean\|long|`0b`|Write straight into `filename` instead of through a temporary file. See below|

**Returns:** a long containing the number of bytes written.

The response is streamed to disk, so the object is not buffered in memory. By default it goes to a
temporary file beside `filename`, which replaces the destination after a complete, successful
write. An S3 or local write failure removes the temporary and leaves an existing destination
untouched.

Because the destination is replaced rather than written in place, a `filename` that is a symbolic
link is replaced by a regular file, leaving the link's target unchanged, and an existing file's
permissions are not kept: the new file gets the process's default permissions.

There is no size limit on a download. The object is written in full to a temporary file in the
destination's directory before it replaces `filename`, so that directory needs free space for the
whole object, in addition to any existing file being replaced. A failed transfer is retried from
the beginning, and nothing is kept if it ultimately fails. For large objects,
[`download`](#download) fetches parts in parallel.

With `inPlace`, the object is written straight into `filename`, as `awss3kdb` did. Use it when
something tracks the download by its final path: its size while in flight, or deleting it after a
crash. The destination directory is still checked before the request is made. Its cost is that a
failure loses the existing file: the SDK opens the destination as soon as a response arrives,
before it knows whether that response is an error, so by then `filename` has been truncated. On
failure it is removed rather than left holding a partial body or S3's error document. A failure
before any response, such as a refused connection, leaves it untouched.

**Example:**

```q
bytes: sdk.getObject[client; "my-bucket"; "data/input.csv"; "download.csv"; ::]
bytes: sdk.getObject[client; "my-bucket"; "data/input.csv"; "download.csv"; ([inPlace: 1b])]
```

## createTransferManager

Creates a transfer manager, used by [`upload`](#upload) and [`download`](#download) to move large
files as parts in parallel. Requires [`initialize`](#initialize) and a client from
[`createClient`](#createclient).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|client|foreign|A client from [`createClient`](#createclient)|
|threads|long|Size of the manager's thread pool, from 1 to 64|
|options|dict|Reserved for future options; pass `::` or an empty dictionary|

**Returns:** a foreign object holding the transfer manager. It keeps its client alive, so the
client handle can be dropped while the transfer manager is in use. It is garbage collected when its
refcount drops to zero, and is invalidated by [`shutDown`](#shutdown) in the same way as a client.

Each thread works on one 5 MB part at a time, with a buffer of its own. The threads and buffers are
created along with the transfer manager, so it allocates 5 MB per thread straight away: 320 MB at
the maximum of 64. A transfer cannot use more connections than its client allows, so threads
beyond the client's `maxConnections`, 25 by default, wait for a connection rather than adding
throughput.

**Example:**

```q
client: sdk.createClient[([region: "eu-west-1"])]
tm: sdk.createTransferManager[client; 6; ::]
```

## upload

Uploads a local file to S3 through a transfer manager, blocking until it finishes. Requires
[`initialize`](#initialize) and a transfer manager from
[`createTransferManager`](#createtransfermanager).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|transferManager|foreign|A transfer manager from [`createTransferManager`](#createtransfermanager)|
|bucket|string\|symbol|Destination bucket|
|key|string\|symbol|Destination object key|
|filename|string\|symbol|Local file to upload|
|options|dict|Reserved for future options; pass `::` or an empty dictionary|

**Returns:** a long containing the number of bytes uploaded.

A file of up to 5 MB is sent with a single `PutObject`. A larger one is sent as a multipart upload,
its 5 MB parts spread across the transfer manager's threads. Unlike [`putObject`](#putobject), this
is not limited to 5 GB, but with 5 MB parts and S3's limit of 10,000 parts per upload, a file can
be at most about 50 GB. The object is stored with the content type `application/octet-stream`.

A `filename` that is not a regular file, such as a directory, or that does not exist is rejected
before an S3 request is made, as is a file too large for 10,000 parts. The file must not change
while it is being uploaded: it is read part by part as the upload proceeds, so a file written to
meanwhile can produce an object that matches no version of it.

If a multipart upload fails, it is aborted before `upload` returns, so the parts already sent do
not stay in S3. They would otherwise be stored, and billed, indefinitely. If the abort also fails,
both errors are reported. A process that dies mid-upload cannot abort it; a bucket lifecycle rule
with `AbortIncompleteMultipartUpload` removes such uploads after a set number of days.

**Example:**

```q
bytes: sdk.upload[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ::]
```

## download

Downloads an S3 object to a local file through a transfer manager, blocking until it finishes.
Requires [`initialize`](#initialize) and a transfer manager from
[`createTransferManager`](#createtransfermanager).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|transferManager|foreign|A transfer manager from [`createTransferManager`](#createtransfermanager)|
|bucket|string\|symbol|Source bucket|
|key|string\|symbol|Source object key|
|filename|string\|symbol|Local destination file|
|options|dict|Options, or `::` for the defaults|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|inPlace|boolean\|long|`0b`|Write straight into `filename` instead of through a temporary file. See below|

**Returns:** a long containing the number of bytes downloaded.

The transfer manager first asks S3 for the object's size, then fetches it in 5 MB ranges spread
across its threads. As with [`getObject`](#getobject), it is written by default to a temporary file
beside `filename`, which replaces the destination after a complete, successful download, so a
failure leaves an existing destination untouched. The same notes apply: the directory needs free
space for the whole object, and a symbolic link or an existing file's permissions are not kept.
After a failure, the temporary is removed once the SDK has closed it: normally before `download`
returns, otherwise moments after.

With `inPlace`, the ranges are written straight into `filename`, as `awss3kdb` did, for callers
that track a download by its final path. The size request comes first, so a missing object or a
permission failure leaves `filename` untouched, since that request fails before anything is
written. A failure part-way through leaves it partial, and
because ranges are written at their own offsets, possibly already at the object's full size with
gaps that read as zeros: in place, the file's size does not show whether the download completed.

**Example:**

```q
bytes: sdk.download[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ::]
bytes: sdk.download[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ([inPlace: 1b])]
```

## uploadAsync

Starts uploading a local file to S3 through a transfer manager, and returns at once. Progress and
the outcome are reported to a callback. Requires [`initialize`](#initialize) and a transfer manager
from [`createTransferManager`](#createtransfermanager).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|transferManager|foreign|A transfer manager from [`createTransferManager`](#createtransfermanager)|
|bucket|string\|symbol|Destination bucket|
|key|string\|symbol|Destination object key|
|filename|string\|symbol|Local file to upload|
|options|dict|Options; `callback` is required|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|callback|function|required|Called with a report dictionary, described below|

**Returns:** `::`.

The upload itself behaves as [`upload`](#upload) does: one `PutObject` up to 5 MB, a parallel
multipart upload above that, at most 10,000 parts, and a failed multipart upload is aborted. The
file must not change until the upload has finished. Anything wrong with the arguments, or a
`filename` that is not a regular file or is too large, is an error from this call; anything that
goes wrong once the upload has started is reported to the callback.

### Reports

The callback is called on q's main thread, whenever q's event loop is idle, with a dictionary:

|Key|Type|Description|
|---|---|---|
|bucket|string|The bucket given|
|key|string|The key given|
|filename|string|The file given|
|direction|symbol|`` `UPLOAD `` or `` `DOWNLOAD ``|
|status|symbol|`` `NOT_STARTED ``, `` `IN_PROGRESS ``, or a final status: `` `COMPLETED ``, `` `FAILED ``, `` `CANCELED `` or `` `ABORTED ``|
|transferred|long|Bytes transferred so far|
|total|long|Size of the object, once known|
|error|string|Why the transfer failed; empty otherwise|

A transfer is reported as it progresses, then exactly once with a final status, after which nothing
more is reported for it. How often it is reported along the way is up to the AWS SDK: with the
current version, once as it starts and after each 5 MB part, in either direction. For an upload,
`transferred` can end slightly above `total`. The keys and values match those `awss3kdb` reported,
so callbacks written for it work unchanged.

An error signalled by the callback is printed to standard error and has no other effect: the
transfer and other transfers' reports carry on. Reports are delivered only while q's event loop
runs, so a script that waits in a loop of its own, or exits, receives none.

The transfer keeps its transfer manager alive until its final report, so the transfer manager's
handle can be dropped while it runs.

**Example:**

```q
done: {[d] if[d[`status] in `COMPLETED`FAILED; -1 d[`key], ": ", string d`status]}
sdk.uploadAsync[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ([callback: done])]
```

## downloadAsync

Starts downloading an S3 object to a local file through a transfer manager, and returns at once.
Progress and the outcome are reported to a callback. Requires [`initialize`](#initialize) and a
transfer manager from [`createTransferManager`](#createtransfermanager).

**Parameters:**

|Name|Type|Description|
|---|---|---|
|transferManager|foreign|A transfer manager from [`createTransferManager`](#createtransfermanager)|
|bucket|string\|symbol|Source bucket|
|key|string\|symbol|Source object key|
|filename|string\|symbol|Local destination file|
|options|dict|Options; `callback` is required|

**Options:**

|Name|Type|Default|Description|
|---|---|---|---|
|callback|function|required|Called with a report dictionary, as for [`uploadAsync`](#reports)|
|inPlace|boolean\|long|`0b`|Write straight into `filename` instead of through a temporary file|

**Returns:** `::`.

The download itself behaves as [`download`](#download) does, including what `inPlace` changes. By
default the temporary file has already replaced `filename` when the callback receives
`` `COMPLETED ``, so the callback can read the file. After any other final status the temporary is
removed once the SDK has closed it: normally before the callback runs, otherwise moments after. An unwritable destination directory is an error from this call; anything that goes wrong
once the download has started is reported to the callback.

Two downloads to the same `filename` at the same time finish in no particular order: either may
be the one left in place.

**Example:**

```q
done: {[d] if[d[`status] in `COMPLETED`FAILED; -1 d[`filename], ": ", string d`status]}
sdk.downloadAsync[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ([callback: done])]
```
