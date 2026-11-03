aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]

// Returns the number of bytes written.
show aws.getObject[client; "my-bucket"; "data/input.csv"; "download.csv"; ::]

aws.shutDown[]
