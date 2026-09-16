aws:use `kx.awssdk;
aws.initialize[]
client: aws.createClient[([region: "eu-west-1"])]
show type client  // 112h, a foreign
aws.shutDown[]
