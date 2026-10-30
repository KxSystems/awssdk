// Tests that need an S3 endpoint: the requests themselves, which test.q cannot make.
//
// Set AWSSDK_TEST_ENDPOINT to an S3-compatible endpoint, and AWSSDK_TEST_BUCKET to a bucket
// that already exists there (default "awssdk-test"); the module cannot create buckets.
// Credentials come from the default chain, e.g. AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY.
// Without AWSSDK_TEST_ENDPOINT the tests are skipped. For example, against MinIO run in Docker,
// with q on the host reaching it through the published port:
//
//   # start MinIO, then create the bucket with the mc client inside its container
//   docker run -d --rm --name minio -p 9000:9000 \
//       -e MINIO_ROOT_USER=minioadmin -e MINIO_ROOT_PASSWORD=minioadmin \
//       quay.io/minio/minio server /data
//   docker exec minio sh -c 'mc alias set l http://localhost:9000 minioadmin minioadmin && mc mb l/awssdk-test'
//
//   # on the host; unset any AWS_SESSION_TOKEN left in the shell, or it is sent with the keys
//   AWSSDK_TEST_ENDPOINT=http://localhost:9000 \
//       AWS_ACCESS_KEY_ID=minioadmin AWS_SECRET_ACCESS_KEY=minioadmin q test_live.q
//
// q loads kx.awssdk from QPATH as usual, so put the build under test first there.
// Objects are written under a prefix unique to the run, and left in the bucket.
//
// kx.test evaluates each assertion in a namespace of its own, while the async callbacks and
// the timer run in the root one, so the state both use is kept in .live.

if[not count getenv `AWSSDK_TEST_ENDPOINT; -1 "AWSSDK_TEST_ENDPOINT is not set: skipping the live tests"; exit 0];

t:use`kx.test;

t.setup[{
    aws::use`kx.awssdk;
    aws.initialize[([loglevel:"OFF"])];
    }]

.live.aws: use`kx.awssdk;
.live.bucket: $[count b: getenv `AWSSDK_TEST_BUCKET; b; "awssdk-test"];
.live.run: "run-", (8 # string first 1?0Ng), "/";
.live.key: {.live.run, x};
.live.c: .live.aws.createClient[([region:"us-east-1"; endpointUrl:getenv `AWSSDK_TEST_ENDPOINT; virtualAddressing:0b])];
.live.tm: .live.aws.createTransferManager[.live.c; 4; ::];
.live.dir: "test_live_files/";
system "mkdir -p ", .live.dir;
.live.file: {.live.dir, x};
.live.small: .live.file "small.txt";
(hsym `$.live.small) 0: enlist "hello";
// Over the transfer manager's 5 MB part size, so it uploads and downloads in parts.
.live.big: .live.file "big.bin";
(hsym `$.live.big) 1: 12000000 # "x"$til 251;
.live.bytes: {read1 hsym `$x};
.live.exists: {not () ~ key hsym `$x};
.live.noBucket: "NoSuchBucket: The specified bucket does not exist";
.live.noKey: "NoSuchKey: The specified key does not exist.";
// A HEAD request, as a transfer manager's download starts with, has no body to name the error.
.live.notFound: "HTTP 404: No response body.";

t.feature `putObject
t.assertMatch[{aws.putObject[.live.c; .live.bucket; .live.key "put/small.txt"; .live.small; ::]};6;::]
t.assertMatch[{aws.putObject[.live.c; `$.live.bucket; `$.live.key "put/symbols.txt"; .live.small; ::]};6;::]
t.assertError[{aws.putObject[.live.c; "awssdk-no-such-bucket"; "key"; .live.small; ::]};.live.noBucket;::]

t.feature `getObject
t.assertMatch[{aws.getObject[.live.c; .live.bucket; .live.key "put/small.txt"; .live.file "got.txt"; ::]};6;::]
t.assertMatch[{.live.bytes .live.file "got.txt"};.live.bytes .live.small;::]
t.assertMatch[{aws.getObject[.live.c; .live.bucket; .live.key "put/small.txt"; .live.file "inplace.txt"; ([inPlace:1b])]};6;::]
t.assertMatch[{.live.bytes .live.file "inplace.txt"};.live.bytes .live.small;::]
// A failed download leaves an existing destination as it was, and creates no file otherwise.
t.assertError[{aws.getObject[.live.c; .live.bucket; .live.key "missing"; .live.file "got.txt"; ::]};.live.noKey;::]
t.assertMatch[{.live.bytes .live.file "got.txt"};.live.bytes .live.small;::]
t.assertError[{aws.getObject[.live.c; .live.bucket; .live.key "missing"; .live.file "absent.txt"; ::]};.live.noKey;::]
t.assertFalse[{.live.exists .live.file "absent.txt"};::]
t.assertError[{aws.getObject[.live.c; .live.bucket; .live.key "missing"; .live.file "absent.txt"; ([inPlace:1b])]};.live.noKey;::]
t.assertFalse[{.live.exists .live.file "absent.txt"};::]

t.feature `listObjects
t.before[{aws.putObject[.live.c; .live.bucket; ; .live.small; ::] each .live.key each ("list/a.txt"; "list/b.txt"; "list/sub/c.txt")}]
t.assertMatch[{0!aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "list/"])]};
    ([] objectKey: .live.key each ("list/a.txt"; "list/b.txt"; "list/sub/c.txt"); size: 6 6 6);::]
t.assertMatch[{exec objectKey from aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "list/"; startAfter: .live.key "list/a.txt"])]};
    .live.key each ("list/b.txt"; "list/sub/c.txt");::]
// marker is the older spelling of startAfter.
t.assertMatch[{exec objectKey from aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "list/"; marker: .live.key "list/a.txt"])]};
    .live.key each ("list/b.txt"; "list/sub/c.txt");::]
t.assertMatch[{count aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "nothing/"])]};0;::]
t.assertMatch[{type aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "nothing/"])]};99h;::]
t.assertError[{aws.listObjects[.live.c; "awssdk-no-such-bucket"; ::]};.live.noBucket;::]

t.feature `listObjectsPaging
// S3 returns at most 1000 keys a page, so 1001 needs a second page.
t.before[{aws.putObject[.live.c; .live.bucket; ; .live.small; ::] each .live.key each "page/" ,/: string til 1001}]
t.assertMatch[{count aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "page/"])]};1001;::]
t.assertMatch[{count aws.listObjectsMetadata[.live.c; .live.bucket; ([prefix: .live.key "page/"])]};1001;::]

t.feature `listObjectsMetadata
t.before[{md::0!aws.listObjectsMetadata[.live.c; .live.bucket; ([prefix: .live.key "list/a.txt"])]}]
t.assertMatch[{cols md};`objectKey`size`lastModified`eTag`storageClass`ownerId;::]
t.assertMatch[{md`objectKey};enlist .live.key "list/a.txt";::]
t.assertMatch[{md`size};enlist 6;::]
t.assertType[{first md`lastModified};"p";::]
t.assertTrue[{.z.p > first md`lastModified};::]
// A single-part upload's ETag is the MD5 of its content, without S3's surrounding quotes.
t.assertMatch[{first md`eTag};raze string md5 "c"$.live.bytes .live.small;::]
t.assertType[{first md`storageClass};"s";::]
t.assertError[{aws.listObjectsMetadata[.live.c; "awssdk-no-such-bucket"; ::]};.live.noBucket;::]

t.feature `upload
t.assertMatch[{aws.upload[.live.tm; .live.bucket; .live.key "tm/small.txt"; .live.small; ::]};6;::]
t.assertMatch[{aws.upload[.live.tm; .live.bucket; .live.key "tm/big.bin"; .live.big; ::]};12000000;::]
t.assertMatch[{aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "tm/big.bin"])][.live.key "tm/big.bin"]`size};12000000;::]
t.assertError[{aws.upload[.live.tm; "awssdk-no-such-bucket"; "key"; .live.small; ::]};.live.noBucket;::]
t.assertError[{aws.upload[.live.tm; "awssdk-no-such-bucket"; "key"; .live.big; ::]};.live.noBucket;::]

t.feature `download
t.assertMatch[{aws.download[.live.tm; .live.bucket; .live.key "tm/big.bin"; .live.file "big.out"; ::]};12000000;::]
t.assertTrue[{.live.bytes[.live.file "big.out"] ~ .live.bytes .live.big};::]
t.assertMatch[{aws.download[.live.tm; .live.bucket; .live.key "tm/big.bin"; .live.file "big.inplace"; ([inPlace:1b])]};12000000;::]
t.assertTrue[{.live.bytes[.live.file "big.inplace"] ~ .live.bytes .live.big};::]
t.assertError[{aws.download[.live.tm; .live.bucket; .live.key "missing"; .live.file "big.out"; ::]};.live.notFound;::]
t.assertTrue[{.live.bytes[.live.file "big.out"] ~ .live.bytes .live.big};::]
t.assertError[{aws.download[.live.tm; .live.bucket; .live.key "missing"; .live.file "absent.bin"; ([inPlace:1b])]};.live.notFound;::]
t.assertFalse[{.live.exists .live.file "absent.bin"};::]

// Async transfers report through q's event loop, so these start the transfers, and a timer
// checks the reports once every final one has arrived.
.live.reports: flip `tag`bucket`key`filename`direction`status`transferred`total`error!
    (`symbol$(); (); (); (); `symbol$(); `symbol$(); `long$(); `long$(); ());
.live.record: {[tag; report] `.live.reports upsert (enlist[`tag]!enlist tag), report};
.live.FINAL: `COMPLETED`FAILED`CANCELED`ABORTED;
.live.TAGS: `upBig`upFail`dlBig`dlInPlace`dlMissing;
.live.finals: {[t] select from .live.reports where tag = t, status in .live.FINAL};

t.feature `asyncStart
t.assertMatch[{aws.uploadAsync[.live.tm; .live.bucket; .live.key "async/big.bin"; .live.big; ([callback: .live.record `upBig])]};::;::]
t.assertMatch[{aws.uploadAsync[.live.tm; "awssdk-no-such-bucket"; "key"; .live.small; ([callback: .live.record `upFail])]};::;::]
t.assertMatch[{aws.downloadAsync[.live.tm; .live.bucket; .live.key "tm/big.bin"; .live.file "async.out"; ([callback: .live.record `dlBig])]};::;::]
t.assertMatch[{aws.downloadAsync[.live.tm; .live.bucket; .live.key "tm/big.bin"; .live.file "async.inplace"; ([callback: .live.record `dlInPlace; inPlace:1b])]};::;::]
t.assertMatch[{aws.downloadAsync[.live.tm; .live.bucket; .live.key "missing"; .live.file "async.absent"; ([callback: .live.record `dlMissing])]};::;::]
// A callback that signals is reported, and does not stop the other transfers' reports.
t.assertMatch[{aws.downloadAsync[.live.tm; .live.bucket; .live.key "put/small.txt"; .live.file "async.small"; ([callback: {[report] if[`COMPLETED ~ report`status; 'boom]}])]};::;::]
// The SDK cannot be shut down while a transfer has yet to report.
t.assertError[{aws.shutDown[::]};"busy";::]

.live.finish: {
    system "t 0";
    t.feature `asyncReports;
    t.assertMatch[{count each .live.finals each .live.TAGS};5#1;::];
    t.assertMatch[{{first exec status from .live.finals x} each .live.TAGS};`COMPLETED`FAILED`COMPLETED`COMPLETED`FAILED;::];
    t.assertMatch[{first[.live.finals `upBig] `direction`total};(`UPLOAD; 12000000);::];
    t.assertMatch[{first[.live.finals `dlBig] `direction`key`total};(`DOWNLOAD; .live.key "tm/big.bin"; 12000000);::];
    t.assertMatch[{first[.live.finals `upFail]`error};.live.noBucket;::];
    t.assertMatch[{first[.live.finals `dlMissing]`error};.live.notFound;::];
    t.assertTrue[{.live.bytes[.live.file "async.out"] ~ .live.bytes .live.big};::];
    t.assertTrue[{.live.bytes[.live.file "async.inplace"] ~ .live.bytes .live.big};::];
    t.assertFalse[{.live.exists .live.file "async.absent"};::];
    t.assertMatch[{aws.listObjects[.live.c; .live.bucket; ([prefix: .live.key "async/big.bin"])][.live.key "async/big.bin"]`size};12000000;::];

    t.feature `shutDown;
    t.assertTrue[{aws.shutDown[::]};::];
    t.assertError[{aws.listObjects[.live.c; .live.bucket; ::]};"uninitialized";::];

    t.teardown[];
    system "rm -rf ", .live.dir;
    report: t.getReport[];
    params: .Q.opt .z.x;
    if[`junitPath in key params; (hsym first `$params`junitPath) 0: t.junitReport report];
    failed: select from report where status <> `pass;
    if[0 < count failed; -1 "\n\nTest failed!\n"; 0N!failed; exit 1];
    exit 0
    };

// Listening keeps q's event loop running even when stdin is closed, as it can be under CI.
system "p 0W";
.live.started: .z.p;
.z.ts: {
    done: all .live.TAGS in exec tag from .live.reports where status in .live.FINAL;
    if[(not done) and 00:01:00 > .z.p - .live.started; :(::)];
    .live.finish[]
    };
system "t 100";
