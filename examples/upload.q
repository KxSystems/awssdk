aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]
tm: aws.createTransferManager[client; 6; ::]

// Files over 5 MB are uploaded as parts in parallel. Returns the number of
// bytes uploaded.
show aws.upload[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ::]

aws.shutDown[]
