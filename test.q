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
t.assertFalse[{aws.shutDown[::]};::]

t.feature `initialize
t.assertTrue[{aws.initialize[::]};::]
t.assertFalse[{aws.initialize[::]};::]

t.feature `getCredentials
t.before[{creds::aws.getCredentials[::]}]
t.assertMatch[{asc key creds};`accessKey`accountId`expiration`secretKey`sessionToken;::]
t.assertType[{creds`expiration};"j";::]

t.feature `createClient
// The client is kept in a global so it is still live for the shutDown feature
// below, which is what exercises the sweep that runs before Aws::ShutdownAPI.
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

// `client` is now a swept handle: q will garbage collect it later, which must
// not double-free it.
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

t.feature `handleReuse
// `client` from the createClient feature is a stale handle: the sweep emptied
// its slot without freeing it. A freed allocation comes straight back on the
// next new, so had the sweep freed slots instead, this stale handle would name
// a client created after it and destroy the wrong one when dropped.
t.before[{aws.initialize[::]; fresh::aws.createClient each 20#enlist ([region:"eu-west-1"])}]
t.assertTrue[{all 112h=type each fresh};::]
t.assertTrue[{client:: (::); .Q.gc[]; all sum 112h=type each fresh};::]
t.assertMatch[{type aws.createClient[::]};112h;::]
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

/exit $[all `pass=report`status;0;1];
