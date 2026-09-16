# Aws-sdk References

These are the API specifications for the aws-sdk module.

## initialize

Initializes the sdk, needs to be called before starting to use other features.

**Parameters:**

|Name|Type|Description|
|---|---|---|
|options|dict|Options to set AWS sdk|

**Example:**

```q
sdk.initialize[::] // use default values: loglevel: info, logPrefix: your q directory

params: ([loglevel: "DEBUG"; logPrefix:"/tmp/sdk_logs/aws_"])
sdk.initialize[params] // override default params
```


## shutDown

Shuts down sdk, needs to be called before exiting the session.

**Example:**

```q
sdk.shutDown[::]
```

## getCredentials

Fetches AWS credentials using the official AWS authentication chain.

**Example:**

```q
credentials: sdk.getCredentials[::]
```
