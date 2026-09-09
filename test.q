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
t.assertFalse[{aws.shutDown[::]};::]

t.feature `initialize
t.assertTrue[{aws.initialize[::]};::]
t.assertFalse[{aws.initialize[::]};::]

t.feature `getCredentials
t.before[{creds::aws.getCredentials[::]}]
t.assertMatch[{asc key creds};`accessKey`accountId`expiration`secretKey`sessionToken;::]
t.assertType[{creds`expiration};"j";::]

t.feature `shutDown
t.assertTrue[{aws.shutDown[::]};::]
t.assertFalse[{aws.shutDown[::]};::]

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

exit $[all `pass=report`status;0;1];
