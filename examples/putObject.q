aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]

// Returns the number of bytes uploaded.
show aws.putObject[client; "my-bucket"; "data/input.csv"; "input.csv"; ::]

aws.shutDown[]
