#!/bin/bash
# filepath: /Users/eswar/Downloads/CppBedrock/keys/gen_server_keys.sh

NUM_SERVERS=${1:-7}  # Default to 7 servers if not specified

for i in $(seq 1 $NUM_SERVERS); do
    echo "Generating keys for server $i..."
    openssl genpkey -algorithm RSA -out server_${i}_private.pem -pkeyopt rsa_keygen_bits:2048
    openssl rsa -pubout -in server_${i}_private.pem -out server_${i}_public.pem
done

echo "Done. Keys generated for $NUM_SERVERS servers."