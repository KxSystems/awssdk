aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]
tm: aws.createTransferManager[client; 6; ::]

// Fetched as 5 MB ranges in parallel, into a temporary file that replaces the
// destination once complete. Returns the number of bytes downloaded.
show aws.download[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ::]

// Written straight into the destination instead.
show aws.download[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ([inPlace: 1b])]

aws.shutDown[]
