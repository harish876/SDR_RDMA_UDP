FROM ubuntu:22.04

# Install dependencies needed for UDP + testing
# We add libisal-dev, which contains the Intel ISA-L library
RUN apt-get update && apt-get install -y \
    g++ \
    make \
    iproute2 \
    iputils-ping \
    net-tools \
    libisal-dev \
    && apt-get clean

# Set working directory
WORKDIR /app

# Copy source files and Makefile into container
COPY . .

# Build the project using your Makefile
RUN make clean || true && make

# Default command: open interactive shell
CMD ["bash"]
