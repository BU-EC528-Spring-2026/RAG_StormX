#!/bin/bash

docker run -it --rm -v $(pwd):/app -v /tmp:/tmp sptag /bin/bash
