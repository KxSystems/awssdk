aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]

// every object in the bucket, as a keyed table of key!size
show aws.listObjects[client; "my-bucket"; ::]

// restricted to one prefix
show aws.listObjects[client; "my-bucket"; ([prefix: "data/2026/"])]

aws.shutDown[]
