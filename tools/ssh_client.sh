#!/bin/bash
ssh -oKexAlgorithms=+diffie-hellman-group-exchange-sha1,diffie-hellman-group1-sha1 -oHostKeyAlgorithms=+ssh-rsa -oPubkeyAcceptedKeyTypes=+ssh-rsa -oCiphers=+aes128-cbc,3des-cbc ezheng@10.0.5.18
