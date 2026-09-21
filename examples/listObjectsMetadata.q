aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]

// key, size, lastModified, eTag, storageClass and owner, in one request
listing: aws.listObjectsMetadata[client; "my-bucket"; ([prefix: "data/2026/"])]
show listing

// the metadata makes questions like these answerable without further calls
show select from 0!listing where lastModified > .z.p - 1D
show exec sum size from listing

aws.shutDown[]
