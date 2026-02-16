# SPTAG Development Environment Guide

## Overview
The SPTAG library requires specific dependencies (GCC 8, Boost, SWIG, etc.) that are difficult to manage locally. To solve this, we use a Docker-based workflow.

I have already built the library inside a container and synced the results (the Release folder and SPTAG.py) back into this repository. You can now use the Docker image to run or recompile the code without worrying about local dependencies.

## 1. Initial Setup
To get started, you must build the Docker image on your own machine. This image contains the environment needed to run the SPTAG binaries.

Run the following command from the root of the repository:
```bash
docker build -t sptag .
```

This may take 10-15 minutes as it compiles the core C++ library.

## 2. Working with the Shared Filesystem
We use a "Volume Mount" to link your local repository folder to the /app folder inside the container. This creates a single filesystem: any change you make in your IDE on the DevBox is instantly visible inside the container, and any binary compiled in the container is saved directly to your DevBox.

To start an interactive session, use:
```bash
docker run -it --rm -v $(pwd):/app sptag /bin/bash
```

### Flag Breakdown:
- -it: Keeps the session interactive so you can type commands.
- --rm: Automatically deletes the container instance (not the image) when you exit, keeping your system clean.
- -v $(pwd):/app: Maps your current directory to /app inside the container.

## 3. Development Workflow

### Python Development
The SPTAG.py and associated .so files are already in the Release folder. You can run Python scripts inside the container environment:
```bash
# Inside the container shell
export PYTHONPATH=$PYTHONPATH:/app/Release
python3 your_script.py
```

### C++ Development and Recompiling
If you modify the C++ source code in the AnnService or Wrappers directories, you must recompile. Because of the volume mount, you do this inside the container using the container's tools:
```bash
# Inside the container shell
cd build
make -j$(nproc)
```
The updated binaries will automatically appear in your local Release folder on the DevBox.

## 4. Why this setup?
1. Persistence: Since the Release folder is on your DevBox disk, your compiled work is not lost when you close Docker.
2. Tooling: You can use your preferred editors on the DevBox while relying on the container for the complex build stack.
3. Consistency: Every teammate is using the exact same compiler and library versions defined in the Dockerfile.

## Troubleshooting
If you enter the container and do not see the Release folder, ensure you are running the docker run command from the root of the SPTAG repository where the Release folder is located.
