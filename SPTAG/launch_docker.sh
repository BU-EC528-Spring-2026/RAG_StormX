#!/bin/bash

docker run -it --rm -v $(pwd):/app -v /app/Release -v /tmp:/tmp -v /mnt/nvme:/mnt/nvme sptag /bin/bash