#!/bin/bash
rm dummy_file* *.pem 
head -c 100000 </dev/urandom > dummy_file
openssl genrsa -out private.pem 2048
openssl rsa -in private.pem -outform PEM -pubout -out public.pem
openssl dgst -binary -sha256 dummy_file > dummy_file.sha256
openssl pkeyutl -sign -in dummy_file.sha256 -inkey private.pem -pkeyopt digest:sha256 -out dummy_file.signature

# RSA-3072 and RSA-4096 fixtures (384- and 512-byte signatures) over the same hash
openssl genrsa -out private_3072.pem 3072
openssl rsa -in private_3072.pem -outform PEM -pubout -out public_3072.pem
openssl pkeyutl -sign -in dummy_file.sha256 -inkey private_3072.pem -pkeyopt digest:sha256 -out dummy_file.signature_3072
openssl genrsa -out private_4096.pem 4096
openssl rsa -in private_4096.pem -outform PEM -pubout -out public_4096.pem
openssl pkeyutl -sign -in dummy_file.sha256 -inkey private_4096.pem -pkeyopt digest:sha256 -out dummy_file.signature_4096
