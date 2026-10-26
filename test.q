t:use`kx.test;

t.setup[{
    aws::use`kx.awssdk;
    // A directory, for the checks that a transfer's source must be a regular file. Writing a
    // file inside it creates it, on every platform.
    aDir::"test_q_dir";
    (hsym `$aDir, "/keep") 1: 0x00;
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
t.assertError[{aws.uploadAsync[::;"bucket";"key";"file";([callback:{x}])]};"uninitialized";::]
t.assertError[{aws.downloadAsync[::;"bucket";"key";"file";([callback:{x}])]};"uninitialized";::]
t.assertFalse[{aws.shutDown[::]};::]

t.feature `initialize
t.assertError[{aws.initialize[1]};"type";::]
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
// Counts and durations may be any q integer type, in a typed or a mixed value list.
t.assertMatch[{type aws.createClient[`connectTimeout`requestTimeout!1000 2000h]};112h;::]
t.assertMatch[{type aws.createClient[`connectTimeout`requestTimeout!1000 2000i]};112h;::]
t.assertMatch[{type aws.createClient[([region:"eu-west-1";connectTimeout:1000h])]};112h;::]
t.assertMatch[{type aws.createClient[([region:"eu-west-1";connectTimeout:1000i])]};112h;::]
t.assertError[{aws.createClient[`connectTimeout`requestTimeout!1000 2000f]};"connectTimeout";::]

t.feature `credentialsFile
t.before[{
    credsFile::"test_credentials";
    writeCreds::{[profiles] (hsym `$credsFile) 0: raze {("[",x,"]"; "aws_access_key_id = id_",x; "aws_secret_access_key = secret_",x)} each profiles}}]
t.after[{hdel hsym `$credsFile}]
t.assertMatch[{writeCreds enlist "default"; type aws.createClient[([credentials:credsFile])]};112h;::]
t.assertMatch[{writeCreds "pq"; type aws.createClient[([credentials:credsFile;profile:"p"])]};112h;::]
// A file holding a single profile, as a mounted secret usually does, needs no profile named.
t.assertMatch[{writeCreds enlist "only"; type aws.createClient[([credentials:credsFile])]};112h;::]
// Several profiles and no [default] is ambiguous, so it is reported rather than guessed at.
t.assertError[{writeCreds "ab"; aws.createClient[([credentials:credsFile])]};"profile";::]
// A profile asked for by name has to exist; it never falls back.
t.assertError[{writeCreds enlist "default"; aws.createClient[([credentials:credsFile;profile:"missing"])]};"profile";::]
t.assertError[{writeCreds enlist "only"; aws.createClient[([credentials:credsFile;profile:"missing"])]};"profile";::]

t.feature `requestFailures
// A client pointed at a port nothing listens on: every request fails at once, with no S3
// server needed, which reaches each function's error reporting. The SDK reads its retry
// settings from the environment when a client is created; a single attempt stops it
// spending 25 seconds backing off before each failure.
t.before[{
    aws.initialize[::];
    retryEnv: `AWS_RETRY_MODE`AWS_MAX_ATTEMPTS;
    saved: getenv each retryEnv;
    retryEnv setenv' ("standard"; enlist "1");
    down::aws.createClient[([region:"us-east-1";endpointUrl:"http://127.0.0.1:9";virtualAddressing:0b;noSignRequest:1b])];
    retryEnv setenv' saved;
    downTm::aws.createTransferManager[down;1;::];
    srcFile::"test_request_source.txt";
    (hsym `$srcFile) 0: enlist "data";
    dstFile::"test_request_destination.txt";
    // The request was made, and failed to connect. The SDK words that according to its HTTP
    // client: curl, as on Linux and macOS, gives "curlCode: 7, Could not connect to server; ..."
    // (7 is CURLE_COULDNT_CONNECT); the Windows clients give exactly the message below.
    // Either is accepted on any platform, since a Windows build can also use curl.
    refused::{[f]
        r: @[f; ::; {x}];
        (10h = type r) and (r like "curlCode: 7, *") or r ~ "Encountered network error when sending http request"}}]
t.after[{hdel hsym `$srcFile; @[hdel; hsym `$dstFile; ::]}]
// The check itself: an argument error is not a failed request.
t.assertFalse[{refused {aws.listObjects[down;42;::]}};::]
t.assertTrue[{refused {aws.listObjects[down;"bucket";::]}};::]
t.assertTrue[{refused {aws.listObjectsMetadata[down;"bucket";::]}};::]
t.assertTrue[{refused {aws.putObject[down;"bucket";"key";srcFile;::]}};::]
t.assertTrue[{refused {aws.upload[downTm;"bucket";"key";srcFile;::]}};::]
// A failed download leaves nothing at the destination, by default or in place.
t.assertTrue[{(refused {aws.getObject[down;"bucket";"key";dstFile;::]}) and () ~ key hsym `$dstFile};::]
t.assertTrue[{(refused {aws.getObject[down;"bucket";"key";dstFile;([inPlace:1b])]}) and () ~ key hsym `$dstFile};::]
t.assertTrue[{(refused {aws.download[downTm;"bucket";"key";dstFile;::]}) and () ~ key hsym `$dstFile};::]
t.assertTrue[{(refused {aws.download[downTm;"bucket";"key";dstFile;([inPlace:1b])]}) and () ~ key hsym `$dstFile};::]

t.feature `shutDown
t.assertTrue[{aws.shutDown[::]};::]
t.assertFalse[{aws.shutDown[::]};::]
t.assertError[{aws.createClient[::]};"uninitialized";::]

t.feature `listObjects
// Argument and option validation only: everything here is rejected before any
// request is made, so these need no S3 endpoint. The listing itself is covered
// by test_live.q, which needs one.
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
t.assertError[{aws.putObject[ot;"bucket";"key";aDir;::]};"unable to read file: test_q_dir";::]
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
t.assertError[{aws.upload[tm;"bucket";"key";aDir;::]};"unable to read file: test_q_dir";::]
t.assertError[{aws.upload[tm;"bucket";"key";"/no/such/awssdk-input";::]};"unable to read file: /no/such/awssdk-input";::]
// Over 10,000 parts of 5 MB is rejected before anything is sent. A sparse
// file has the size without the disk space.
t.assertError[{system "truncate -s 60G awssdk_test_sparse"; r:@[aws.upload[tm;"bucket";"key";;::]; "awssdk_test_sparse"; {hdel `:awssdk_test_sparse; 'x}]; hdel `:awssdk_test_sparse; r};"unable to upload file: awssdk_test_sparse needs more than 10000 parts of 5 MB";::]
t.assertError[{aws.download[tm;"bucket";"key";"/no/such/awssdk-dir/output";::]};"unable to write file: /no/such/awssdk-dir/output";::]
t.assertError[{aws.download[tm;"bucket";"key";"/no/such/awssdk-dir/output";([inPlace:1b])]};"unable to write file: /no/such/awssdk-dir/output";::]

t.feature `asyncTransfer
// Everything an async transfer rejects before starting, so none of these
// reaches S3. Reports arrive only once q's event loop runs, after this script,
// so delivery is covered by test_live.q.
t.before[{tm::aws.createTransferManager[aws.createClient[::];2;::]}]
// The callback is required, and has to be something q can apply.
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"file";::]};"callback";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"file";()!()]};"callback";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"file";([callback:42])]};"callback";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"file";([callback:(::)])]};"callback";::]
t.assertError[{aws.uploadAsync[tm;"bucket";"key";"file";([callback:"f"])]};"callback";::]
t.assertError[{aws.uploadAsync[tm;"bucket";"key";"file";1 2 3]};"type";::]
t.assertError[{aws.uploadAsync[tm;"bucket";"key";"file";([callback:{x};inPlace:1b])]};"inPlace";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"file";([callback:{x};partSize:1])]};"partSize";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"file";([callback:{x};inPlace:"yes"])]};"inPlace";::]
t.assertError[{aws.uploadAsync[42;"bucket";"key";"file";([callback:{x}])]};"transferManager";::]
t.assertError[{aws.downloadAsync[aws.createClient[::];"bucket";"key";"file";([callback:{x}])]};"transferManager";::]
t.assertError[{aws.uploadAsync[tm;42;"key";"file";([callback:{x}])]};"bucket";::]
t.assertError[{aws.downloadAsync[tm;"bucket";42;"file";([callback:{x}])]};"key";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";42;([callback:{x}])]};"filename";::]
// The same local checks as the blocking functions, raised at the call rather
// than reported through the callback.
t.assertError[{aws.uploadAsync[tm;"bucket";"key";aDir;([callback:{x}])]};"unable to read file: test_q_dir";::]
t.assertError[{aws.uploadAsync[tm;"bucket";"key";"/no/such/awssdk-input";([callback:{x}])]};"unable to read file: /no/such/awssdk-input";::]
t.assertError[{system "truncate -s 60G awssdk_test_sparse"; r:@[aws.uploadAsync[tm;"bucket";"key";;([callback:{x}])]; "awssdk_test_sparse"; {hdel `:awssdk_test_sparse; 'x}]; hdel `:awssdk_test_sparse; r};"unable to upload file: awssdk_test_sparse needs more than 10000 parts of 5 MB";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"/no/such/awssdk-dir/output";([callback:{x}])]};"unable to write file: /no/such/awssdk-dir/output";::]
t.assertError[{aws.downloadAsync[tm;"bucket";"key";"/no/such/awssdk-dir/output";([callback:{x};inPlace:1b])]};"unable to write file: /no/such/awssdk-dir/output";::]

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
t.assertError[{aws.upload[oldTm;"bucket";"key";aDir;::]};"transferManager";::]
t.assertError[{aws.download[oldTm;"bucket";"key";"file";::]};"transferManager";::]
t.assertError[{aws.uploadAsync[oldTm;"bucket";"key";aDir;([callback:{x}])]};"transferManager";::]
t.assertError[{aws.downloadAsync[oldTm;"bucket";"key";"file";([callback:{x}])]};"transferManager";::]
// Dropping the old handles frees only their own slots: every fresh handle
// still resolves. A manager lookup precedes the file check, so the file error
// shows the manager was found.
t.assertTrue[{oldClient::(::); oldTm::(::); all 112h=type each aws.createTransferManager[;1;::] each freshClients};::]
t.assertError[{oldClient::(::); oldTm::(::); aws.upload[;"bucket";"key";aDir;::] each freshTms};"unable to read file: test_q_dir";::]
t.assertTrue[{aws.shutDown[::]};::]

t.feature `initializeWithParams
t.assertError[{aws.initialize[([loglevel:"asdf"])]};"loglevel";::]
t.assertError[{aws.initialize[([loglevel:1])]};"loglevel";::]
t.assertTrue[{aws.initialize[([loglevel:"DEBUG"])]};::]
t.assertTrue[{aws.shutDown[::]};::]

t.feature `logPrefix
t.before[{logDir::"test_log"; logPrefix::logDir,"/aws_log_"; rmLogDir logDir}]
t.after[{rmLogDir logDir}]
t.assertError[{aws.initialize[([logPrefix:1234])]};"type";::]
// A log directory that cannot be created, here because its parent is a file.
t.assertError[{`:test_log_file 1: 0x00; r: @[aws.initialize; ([logPrefix:"test_log_file/sub/aws_"]); {x}]; hdel `:test_log_file; 'r};"logPrefix";::]
t.assertTrue[{aws.initialize[([logPrefix:logPrefix])]};::]
t.assertTrue[{aws.shutDown[::]};::]
t.assertFalse[{key[hsym `$logDir] ~ ()};::] // dir exists
t.assertTrue[{any (string key[hsym `$logDir]) like "aws_log_*"}]

t.feature `logTo
t.before[{logToScript::"logto_test.q"; logToDir::"logto_dir"; rmLogDir logToDir}]
t.after[{hdel hsym `$logToScript; rmLogDir logToDir}]
t.assertError[{aws.initialize[([logTo:"stdout"])]};"logTo";::]
t.assertError[{aws.initialize[([logTo:1])]};"type";::]
// The SDK's log reaches standard output, and logPrefix is ignored: no log directory is created.
// Run in a fresh process, since the log destination is fixed when the SDK is initialized.
t.assertTrue[{
    (hsym `$logToScript) 0: ("aws:use`kx.awssdk;";
        "aws.initialize[([loglevel:\"INFO\"; logTo:`console; logPrefix:\"", logToDir, "/aws_\"])];";
        "exit 0");
    // Through a shell: system would take a command starting with "q" for q's own \q.
    out: system $[.z.o like "w*"; "cmd /c q "; "env q "], logToScript, " -q 2>&1";
    (any out like "*Initiate AWS SDK*") and () ~ key hsym `$logToDir};::]

t.teardown[]  // runs the final feature's after block
hdel `:test_q_dir/keep;
hdel `:test_q_dir;

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
