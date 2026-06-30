#!/bin/bash

if [ "$1" == "" ]; then
    echo "usage: $0 <ip_addr>"
    exit 1;
fi

curl -X POST -H "Content-Type: application/octet-stream" --data-binary @.pio/build/lolin32_lite/firmware.bin http://$1/api/ota/update
