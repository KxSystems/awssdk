t:use`kx.test;

t.setup[{
    aws::use`kx.awssdk;
    rmLogDir::{[d]
        h:hsym `$d;
        if[not 11h ~ type k:key h; :(::)];
        hdel each hsym `$(d,"/"),/:string k;
        hdel h; }}]

t.feature `uninitialized
t.assertError[{aws.getCredentials[::]};"uninitialized";::]
t.assertError[{aws.createClient[::]};"uninitialized";::]
t.assertError[{aws.listObjects[::;"bucket";::]};"uninitialized";::]
t.assertError[{aws.listObjectsMetadata[::;"bucket";::]};"uninitialized";::]
t.assertError[{aws.putObject[::;"bucket";"key";"file";::]};"uninitialized";::]
t.assertError[{aws.getObject[::;"bucket";"key";"file";::]};"uninitialized";::]
t.assertError[{aws.createTransferManager[::;1;::]};"uninitialized";::]
t.assertError[{aws.upload[::;"bucket";"key";"file";::]};"uninitialized";::]
t.assertError[{aws.download[::;"bucket";"key";"file";::]};"uninitialized";::]
t.assertFalse[{aws.shutDown[::]};::]

t.feature `initialize
t.assertTrue[{aws.initialize[::]};::]
t.assertFalse[{aws.initialize[::]};::]

t.feature `getCredentials
t.before[{creds::aws.getCredentials[::]}]
t.assertMatch[{asc key creds};`accessKey`accountId`expiration`secretKey`sessionToken;::]
t.assertType[{creds`expiration};"j";::]

t.feature `createClient
t.before[{client::aws.createClient[([region:"eu-west-1"])]}]
t.assertMatch[{type client};112h;::]
t.assertMatch[{type aws.createClient[::]};112h;::]
t.assertMatch[{type aws.createClient[([region:`$"eu-west-1"])]};112h;::]
t.assertMatch[{type aws.createClient[([endpointUrl:"http://localhost:9000";virtualAddressing:0])]};112h;::]
t.assertMatch[{type aws.createClient[([noSignRequest:1b;virtualAddressing:0b])]};112h;::]
// An unknown or mis-spelled option is reported by name rather than ignored:
// a silently dropped `region` would send requests to the wrong place.
t.assertError[{aws.createClient[([badOption:"x"])]};"badOption";::]
t.assertError[{aws.createClient[([Region:"eu-west-1"])]};"Region";::]
t.assertError[{aws.createClient[([region:1234])]};"region";::]
t.assertError[{aws.createClient[([endpointUrl:99])]};"endpointUrl";::]
t.assertError[{aws.createClient[([virtualAddressing:"yes"])]};"virtualAddressing";::]
t.assertError[{aws.createClient[([credentials:42])]};"credentials";::]
t.assertError[{aws.createClient[([profile:42])]};"profile";::]
t.assertError[{aws.createClient[([caFile:42])]};"caFile";::]
t.assertError[{aws.createClient[([credentials:"/nonexistent/credentials"])]};"credentials";::]
// A profile that is asked for by name has to exist; it never falls back.
t.assertError[{aws.createClient[([credentials:"/nonexistent/credentials";profile:"p"])]};"credentials";::]
t.assertError[{aws.createClient[([caPath:7])]};"caPath";::]
t.assertError[{aws.createClient[([verifySsl:"yes"])]};"verifySsl";::]
// A count or duration the SDK would misread as unsigned is rejected outright.
t.assertError[{aws.createClient[([connectTimeout:0])]};"connectTimeout";::]
t.assertError[{aws.createClient[([requestTimeout:-1])]};"requestTimeout";::]
t.assertError[{aws.createClient[([maxConnections:0])]};"maxConnections";::]
// A q long is 64-bit but maxConnections is an unsigned field, so an oversized
// value has to be rejected rather than wrapped: 2^32 would land as 0.
t.assertError[{aws.createClient[([maxConnections:4294967296])]};"maxConnections";::]
t.assertMatch[{type aws.createClient[([maxConnections:4294967295])]};112h;::]
t.assertMatch[{type aws.createClient[([verifySsl:0b;caPath:"/etc/ssl/certs";requestTimeout:0;maxConnections:100])]};112h;::]
// profile only selects within an explicit credentials file; alone it is an error
// rather than being silently ignored. AWS_PROFILE covers the default chain.
t.assertError[{aws.createClient[([profile:"default"])]};"profile";::]
// noSignRequest discards credential selection, so the same options are ignored
// rather than rejected there -- though their types are still checked.
t.assertMatch[{type aws.createClient[([noSignRequest:1b;profile:"default"])]};112h;::]
t.assertError[{aws.createClient[([noSignRequest:1b;profile:42])]};"profile";::]
t.assertError[{aws.createClient[1 2 3]};"type";::]

t.feature `shutDown
t.assertTrue[{aws.shutDown[::]};::]
t.assertFalse[{aws.shutDown[::]};::]
t.assertError[{aws.createClient[::]};"uninitialized";::]

t.feature `listObjects
// Argument and option validation only: everything here is rejected before any
// request is made, so these need no S3 endpoint. The listing itself is covered
// by the live tests against MinIO.
t.before[{aws.initialize[::];lo::aws.createClient[::]}]
// `::` options reach get_client, which proves dict_unknown_key and the three
// dict_find_str calls all ran against a 101h generic null without treating it
// as a dictionary. A real dictionary is still validated -- see the `depth` case
// below.
t.assertError[{aws.listObjects[42;"bucket";::]};"client";::]
t.assertError[{aws.listObjects[42;"bucket";()!()]};"client";::]
t.assertError[{aws.listObjects[(1;2);"bucket";::]};"client";::]
t.assertError[{aws.listObjects[lo;42;::]};"bucket";::]
t.assertError[{aws.listObjects[lo;"bucket";1 2 3]};"type";::]
t.assertError[{aws.listObjects[lo;"bucket";([depth:1])]};"depth";::]
t.assertError[{aws.listObjects[lo;"bucket";([prefix:42])]};"prefix";::]
// marker is an alias for startAfter, so asking for both needs a precedence
// rule nobody wanted; it is an error, reported as the deprecated spelling.
t.assertError[{aws.listObjects[lo;"bucket";([startAfter:"a";marker:"b"])]};"marker";::]
// listObjectsMetadata takes the same arguments and options, so it rejects the
// same things; only the columns it builds differ.
t.assertError[{aws.listObjectsMetadata[42;"bucket";::]};"client";::]
t.assertError[{aws.listObjectsMetadata[lo;42;::]};"bucket";::]
t.assertError[{aws.listObjectsMetadata[lo;"bucket";1 2 3]};"type";::]
t.assertError[{aws.listObjectsMetadata[lo;"bucket";([depth:1])]};"depth";::]

t.feature `objectTransfer
// Argument validation and local upload failure need no S3 endpoint.
t.before[{ot::aws.createClient[::]}]
t.assertError[{aws.putObject[42;"bucket";"key";"file";::]};"client";::]
t.assertError[{aws.getObject[42;"bucket";"key";"file";::]};"client";::]
t.assertError[{aws.putObject[ot;42;"key";"file";::]};"bucket";::]
t.assertError[{aws.putObject[ot;"bucket";42;"file";::]};"key";::]
t.assertError[{aws.putObject[ot;"bucket";"key";42;::]};"filename";::]
t.assertError[{aws.getObject[ot;"bucket";42;"file";::]};"key";::]
t.assertError[{aws.getObject[ot;"bucket";"key";42;::]};"filename";::]
t.assertError[{aws.putObject[ot;"bucket";"key";"file";1 2 3]};"type";::]
t.assertError[{aws.putObject[ot;"bucket";"key";"file";([contentType:"text/plain"])]};"contentType";::]
t.assertError[{aws.getObject[ot;"bucket";"key";"file";([versionId:"1"])]};"versionId";::]
// An empty options dictionary is equivalent to ::. The missing input is
// rejected before an HTTP request is attempted.
t.assertError[{aws.putObject[ot;"bucket";"key";"/no/such/awssdk-input";()!()]};"unable to read file: /no/such/awssdk-input";::]
// A directory opens as a stream on Linux and claims a size of INT64_MAX, so it
// has to be rejected as not a regular file rather than sent.
t.assertError[{aws.putObject[ot;"bucket";"key";"/tmp";::]};"unable to read file: /tmp";::]
// The destination directory is probed before the request, so a local path
// failure also avoids an unnecessary network call.
t.assertError[{aws.getObject[ot;"bucket";"key";"/no/such/awssdk-dir/output";::]};"unable to write file: /no/such/awssdk-dir/output";::]
// In place, the directory is probed the same way, without touching the
// destination itself.
t.assertError[{aws.getObject[ot;"bucket";"key";"/no/such/awssdk-dir/output";([inPlace:1b])]};"unable to write file: /no/such/awssdk-dir/output";::]
t.assertError[{aws.getObject[ot;"bucket";"key";"file";([inPlace:"yes"])]};"inPlace";::]

t.feature `transferManager
// Creating a manager and validating transfer arguments need no S3 endpoint.
t.before[{tc::aws.createClient[::]; tm::aws.createTransferManager[tc;2;::]}]
t.assertMatch[{type tm};112h;::]
t.assertMatch[{type aws.createTransferManager[tc;64;()!()]};112h;::]
// A pool of no threads could never run a transfer.
t.assertError[{aws.createTransferManager[tc;0;::]};"threads";::]
t.assertError[{aws.createTransferManager[tc;-1;::]};"threads";::]
// Each thread allocates its own 5 MB part buffer up front.
t.assertError[{aws.createTransferManager[tc;65;::]};"threads";::]
t.assertError[{aws.createTransferManager[tc;0Nj;::]};"threads";::]
t.assertError[{aws.createTransferManager[tc;2i;::]};"threads";::]
t.assertError[{aws.createTransferManager[42;2;::]};"client";::]
t.assertError[{aws.createTransferManager[tc;2;1 2 3]};"type";::]
t.assertError[{aws.createTransferManager[tc;2;([partSize:1])]};"partSize";::]
// A client is not a transfer manager, nor the other way round.
t.assertError[{aws.upload[tc;"bucket";"key";"file";::]};"transferManager";::]
t.assertError[{aws.download[42;"bucket";"key";"file";::]};"transferManager";::]
t.assertError[{aws.listObjects[tm;"bucket";::]};"client";::]
t.assertError[{aws.upload[tm;42;"key";"file";::]};"bucket";::]
t.assertError[{aws.upload[tm;"bucket";42;"file";::]};"key";::]
t.assertError[{aws.upload[tm;"bucket";"key";42;::]};"filename";::]
t.assertError[{aws.download[tm;42;"key";"file";::]};"bucket";::]
t.assertError[{aws.download[tm;"bucket";42;"file";::]};"key";::]
t.assertError[{aws.download[tm;"bucket";"key";42;::]};"filename";::]
t.assertError[{aws.upload[tm;"bucket";"key";"file";1 2 3]};"type";::]
t.assertError[{aws.download[tm;"bucket";"key";"file";1 2 3]};"type";::]
t.assertError[{aws.upload[tm;"bucket";"key";"file";([inPlace:1b])]};"inPlace";::]
t.assertError[{aws.download[tm;"bucket";"key";"file";([partSize:1])]};"partSize";::]
t.assertError[{aws.download[tm;"bucket";"key";"file";([inPlace:"yes"])]};"inPlace";::]
// The manager opens the upload source itself, so it is checked first.
t.assertError[{aws.upload[tm;"bucket";"key";"/tmp";::]};"unable to read file: /tmp";::]
t.assertError[{aws.upload[tm;"bucket";"key";"/no/such/awssdk-input";::]};"unable to read file: /no/such/awssdk-input";::]
// Over 10,000 parts of 5 MB is rejected before anything is sent. A sparse
// file has the size without the disk space.
t.assertError[{system "truncate -s 60G awssdk_test_sparse"; r:@[aws.upload[tm;"bucket";"key";;::]; "awssdk_test_sparse"; {hdel `:awssdk_test_sparse; 'x}]; hdel `:awssdk_test_sparse; r};"unable to upload file: awssdk_test_sparse needs more than 10000 parts of 5 MB";::]
t.assertError[{aws.download[tm;"bucket";"key";"/no/such/awssdk-dir/output";::]};"unable to write file: /no/such/awssdk-dir/output";::]
t.assertError[{aws.download[tm;"bucket";"key";"/no/such/awssdk-dir/output";([inPlace:1b])]};"unable to write file: /no/such/awssdk-dir/output";::]

t.feature `staleHandles
// Handles that outlive a shutDown. kx.test deletes t.before globals at every
// feature boundary, so they are created and swept within this feature: a
// handle from an earlier one would be gone, and since q's error for an
// undefined `client` is 'client, an assertion on it would pass regardless.
//
// shutDown empties the old handles' slots without freeing them, so a fresh
// handle can never land on a stale one's address. That reuse cannot be forced
// from q -- building an SDK object makes many small allocations, which take a
// freed slot first -- so what is checked is the deterministic part: stale
// handles are rejected rather than resolved, which would crash, and dropping
// them leaves every fresh handle intact.
t.before[{
    aws.initialize[::];
    oldClient::aws.createClient[::];
    oldTm::aws.createTransferManager[oldClient;1;::];
    aws.shutDown[::];
    aws.initialize[::];
    freshClients::aws.createClient each 20#enlist ([region:"eu-west-1"]);
    freshTms::aws.createTransferManager[;1;::] each 5#freshClients
    }]
t.assertError[{aws.listObjects[oldClient;"bucket";::]};"client";::]
t.assertError[{aws.putObject[oldClient;"bucket";"key";"file";::]};"client";::]
t.assertError[{aws.getObject[oldClient;"bucket";"key";"file";::]};"client";::]
t.assertError[{aws.createTransferManager[oldClient;1;::]};"client";::]
t.assertError[{aws.upload[oldTm;"bucket";"key";"/tmp";::]};"transferManager";::]
t.assertError[{aws.download[oldTm;"bucket";"key";"file";::]};"transferManager";::]
// Dropping the old handles frees only their own slots: every fresh handle
// still resolves. A manager lookup precedes the file check, so the file error
// shows the manager was found.
t.assertTrue[{oldClient::(::); oldTm::(::); all 112h=type each aws.createTransferManager[;1;::] each freshClients};::]
t.assertError[{oldClient::(::); oldTm::(::); aws.upload[;"bucket";"key";"/tmp";::] each freshTms};"unable to read file: /tmp";::]
t.assertTrue[{aws.shutDown[::]};::]

t.feature `initializeWithParams
t.assertError[{aws.initialize[([loglevel:"asdf"])]};"loglevel";::]
t.assertTrue[{aws.initialize[([loglevel:"DEBUG"])]};::]
t.assertTrue[{aws.shutDown[::]};::]

t.feature `logPrefix
t.before[{logDir::"test_log"; logPrefix::logDir,"/aws_log_"; rmLogDir logDir}]
t.after[{rmLogDir logDir}]
t.assertError[{aws.initialize[([logPrefix:1234])]};"type";::]
t.assertTrue[{aws.initialize[([logPrefix:logPrefix])]};::]
t.assertTrue[{aws.shutDown[::]};::]
t.assertFalse[{key[hsym `$logDir] ~ ()};::] // dir exists
t.assertTrue[{any (string key[hsym `$logDir]) like "aws_log_*"}]

t.teardown[]  // runs the final feature's after block

report: t.getReport[];
params:.Q.opt .z.x;
if[`junitPath in key params;
    r: t.junitReport report;
    h:hsym first `$params`junitPath;
    h 0: r;
  ]

failed: select from report where status <> `pass;
if[0<count failed;
  -1"\n\nTest failed!\n";
  0N!failed;
  exit 1;
  ];

exit 0;
