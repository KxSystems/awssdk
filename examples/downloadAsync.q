aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]
tm: aws.createTransferManager[client; 6; ::]

// By default the file is already in place when COMPLETED is reported.
done: {[d]
    if[d[`status] in `COMPLETED`FAILED`CANCELED;
        -1 d[`filename], ": ", (string d`status), " ", d`error;
        aws.shutDown[]]}

// Returns at once; the reports arrive after this script finishes.
aws.downloadAsync[tm; "my-bucket"; "data/input.parquet"; "input.parquet"; ([callback: done])]
