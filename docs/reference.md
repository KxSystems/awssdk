# Aws-sdk References

These are the API specifications for the aws-sdk module.

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

**Returns:** `1b` if this call shut down an initialized SDK, `0b` if there was nothing to shut down. Any [`createClient`](#createclient) handle still held becomes inert — see [`createClient`](#createclient)'s **Returns**.

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
