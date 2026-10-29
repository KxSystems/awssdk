# awssdk

kdb-x module `kx.awssdk`: a C++ wrapper of the AWS SDK for C++. Functions are exported in
`src/module.cpp` and documented in `docs/reference.md`.

## Build

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=$CONDA_PREFIX   # once
cmake --build build                                     # produces build/awssdk.li64.so
```

## Test

q loads `kx.awssdk` from `QPATH` (default `~/.kx/mod`), so put the new build first:

```sh
mkdir -p /tmp/awssdk-mod/kx && cp build/awssdk.li64.so /tmp/awssdk-mod/kx/
export QPATH=/tmp/awssdk-mod:$HOME/.kx/mod
q test.q            # no network needed
q test_live.q       # needs an S3 endpoint; see the comment at the top of the file
```

## Rules

- Call the module only from q's main thread.
- Keep `docs/reference.md` in step with any change to a function's behaviour.
- Keep `test.q` free of network access; anything needing S3 goes in `test_live.q`.

## Design decisions

- Downloads go through a temporary file renamed on success; `inPlace` writes directly.
- Each async transfer gives its callback exactly one final report.
- `shutDown` refuses with `'busy` while async transfers run; the exit handler only flushes the log.
- The SDK's libstdc++ must be the one shared, dynamically linked copy: awss3kdb's SDK patches
  worked around two copies in one process, and are not needed here.
