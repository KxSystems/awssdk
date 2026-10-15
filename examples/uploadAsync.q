aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]
tm: aws.createTransferManager[client; 6; ::]

// Called on q's main thread while q's event loop is idle: once per status
// change, then once with a final status.
done: {[d]
    if[d[`status] in `COMPLETED`FAILED`CANCELED;
        -1 d[`key], ": ", (string d`status), " ", d`error;
        aws.shutDown[]]}

// Returns at once; the reports arrive after this script finishes.
aws.uploadAsync[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ([callback: done])]
