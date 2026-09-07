# Aws-sdk kdb-x installation

Aws-sdk is written as a module, under KDB-X's module framework. Though modules can be loaded from anywhere if added to your `$QPATH`, we recommend installing under a `kx` folder within your `$QPATH`. This is to avoid name clashes with other user defined modules, as well as providing a name for other KX modules to cross reference each other.

## Installing from qmamba

qmamba is a package manager for kdb-x. It is currently available in a private preview capacity. You are welcome to try it and provide feedback.
Follow the install instructions for [qmamba](https://github.com/KxSystems/qmamba/blob/main/README.md#installation).

```q
qmamba:use`kx.qmamba
qmamba.create "myenv"
qmamba.activate "myenv"
qmamba.install `SPECS`CHANNEL!(enlist "q-kx-aws-sdk";("kx";"conda-forge"))
aws:use`kx.awssdk
```

## Installing a Release

It is recommended that a user install this module through a release. 

[Download a release](https://github.com/KxSystems/awssdk/releases) and then unzip to your module directory. The following example assumes the default install location for KDB-X.

```
unzip aws-sdk-l64.zip -d ~/.kx/mod
```

## Installing from Source

### Installing dependencies

Follow the instruction on [AWS documentation](https://docs.aws.amazon.com/sdk-for-cpp/v1/developer-guide/sdk-from-source.html) to 
install `aws sdk cpp` headers and libraries.

```bash
cd aws-sdk-cpp
export AWS_SDK_PATH=<path/to/install>
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTING=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_ONLY="s3;sts;s3-crt;transfer;iam" -DCMAKE_INSTALL_PREFIX=${AWS_SDK_PATH} ..
cmake --build . --config Release --target install 
```

Note: your `$AWS_SDK_PATH` must be available during build time. If you use specific location, you need to add `$AWS_SDK_PATH` to 
your `LD_LIBRARY_PATH`

### Installing Aws-sdk

Afer building it move `awssdk.<platform>.[so|dll]` into your module directory, under `kx`. The following example assumes the default install location for KDB-X.

```bash
git clone https://github.com/KxSystems/awssdk/
cd awssdk
export INSTALL_PATH=$(pwd)/out/kx
mkdir build
cd build
cmake .. -DCMAKE_PREFIX_PATH=$AWS_SDK_PATH -DCMAKE_INSTALL_PREFIX=$INSTALL_PATH -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release --target install
cp -r $INSTALL_PATH ~/.kx/mod/
```

## Next Steps

Now from anywhere you can import awssdk.

```q
q)aws:use`kx.awssdk
```

You're ready to check out some of the examples we've provided [here](../examples/) and the [reference](reference.md) to get started
