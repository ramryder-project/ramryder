<p align="center">
  <img src="logos/logo_small_trim.png" alt="RamRyder Logo" width="80%" />
</p>

# About

RamRyder is a software-defined elastic memory system for cloud virtual machines. Its core idea is to manage and allocate memory channels in software, allowing users to control the memory capacity and bandwidth of each virtual machine based on application demands.

The main components of RamRyder include a user-space resource manager, a hypervisor extended from QEMU, and a guest Linux kernel.

# Quick Start

This section describes how to quickly set up RamRyder and launch a VM.

## Build Project

1\. Get Source Code
```bash
git clone --recurse-submodules git@github.com:ramryder-project/ramryder.git
```

2\. Install Dependencies
```bash
cd ramryder
./scripts/pkgdep.sh
```

3\. Build Resource Manager
```bash
# Use --arch-cpu-amd if you run on an AMD server. Default: Intel.
./configure [--arch-cpu-amd]
make
```

4\. Build QEMU
```bash
cd qemu
mkdir -p build
cd build
../configure --enable-kvm --target-list=x86_64-softmmu --enable-slirp
make -j$(nproc)
cd ../..
```

## Configure Resources

1\. Configure Hardware

RamRyder supports multiple memory hardware types (for example, DIMM, PMEM, and CXL). Before configuring the resource manager, prepare and expose your memory devices correctly on the host. For hardware-specific setup steps, see [Document - Hardware Support](https://ramryder-project.github.io/docs/hardware-support/overview).

2\. Set Configuration File

Create `src/elesticmm.conf` (or copy from `src/elasticmm_default.conf`) and
configure your memory devices:

```ini
[global]
# size in MB
segment_size_mb 128
monitor_interval_second 1

[devices]
# the combination of tier id and dev id should be unique
dev path=/dev/dax0.0 size_mb=20480 tier_id=0 dax_id=0
dev path=/dev/dax1.0 size_mb=20480 tier_id=0 dax_id=1

[clouddb]
enable_clouddb false
influxdb_url <url>
influxdb_token <token>
use_proxy false
proxy_addr <proxy addr>
```

The minimum required configuration is the `[devices]` section. Each memory
device should be exposed as a DAX device under `/dev` so it can be managed in
user space.

For each DAX device:
- Use `tier_id=0` for local memory (DIMM).
- Use `tier_id=1` for CXL memory.
- Keep `dax_id` unique within the same `tier_id`.


## Start Process
1\. Start Resource Manager

```bash
cd src
sudo ./resource_manager
```

`sudo` is required because the resource manager reads host performance counters.

2\. Get VM Image

We provide a clean Ubuntu VM image: [Download Link](https://drive.google.com/file/d/1DASrFSRzh7dV2UX0fINgHhx10W13yZdz/view?usp=sharing).

```bash
tar -xf nvcloud-image-clean.tar.xz
```

Then refer to `readme.txt` inside the package for login information.

3\. Create VM

All VM operations are managed by `admin/ramryder_cli`. You can use this tool to query resource allocations, allocate resources, and create VMs. Use `ramryder_cli --help` to check usage.

Create a VM with 32 vCPU and DIMM + CXL memory:
```bash
./admin/ramryder_cli create-vm \
  --cpu-set 0-15,128-143 \
  --memory 100G \
  --channels 4 \
  --cxl-memory 50G \
  --cxl-channels 2 \
  --image /path/to/nvcloud-image-clean.qcow2
```

Important Behavior:
- `--memory/--channels` are for local memory (DIMM, tier 0).
- `--cxl-memory/--cxl-channels` are for CXL memory (tier 1).
- `--image` sets the VM qcow2 path (default: `~/images/nvcloud-image-clean.qcow2`).
- The SSH forwarding port is generated automatically.
- Use `ssh -p <port> <user>@localhost` to log in to the VM.

## Update Guest Kernel
After the VM is ready, log in to the VM and then update the guest kernel as follows.

1\. Get Source Code

```bash
git clone git@github.com:ramryder-project/ramos.git
cd ramos
```

2\. Prepare

Install the required build dependencies first.

Run all remaining commands in the kernel source tree:

```bash
sudo apt-get update
sudo apt-get install build-essential libncurses5 libncurses5-dev bin86 kernel-package libssl-dev bison flex libelf-dev dwarves
```

3\. Configure Kernel

Start from the current system configuration:

```bash
cp /boot/config-$(uname -r) .config
make olddefconfig
```

Then open the configuration menu:

```bash
make menuconfig
```

In `make menuconfig`, enable the following RAMOS options under `General setup`:
- `RAMOS NUMA abstraction support`
- `RAMOS debug mode` (optional, for more verbose log output)

4\. Build and Install

Use the following commands for a full kernel build and installation:

```bash
make -j$(nproc)
make -j$(nproc) modules
sudo make INSTALL_MOD_STRIP=1 modules_install
sudo make install
```

Then reboot the VM and select the new kernel `Linux 6.3.0-ramos+`.

Note that `INSTALL_MOD_STRIP=1` removes debug symbols from kernel modules. This reduces build time and saves storage space, but you may want to keep debug symbols if you plan to use `gdb`.

# Detailed Instructions
This section provides detailed instructions to construct same experimental setups and reproduce experimental results described in the paper.


## Set Up VMs
1\. Start Resource Manager
```bash
cd src
sudo ./resource_manager
```

2\. Create VM 1 with 16 vCPU and 32GB DIMM on 1 channel:
```bash
./admin/ramryder_cli create-vm \
  --cpu-set 0-7,128-135 \
  --memory 32G \
  --channels 1 \
  --image /path/to/nvcloud-image-vm1.qcow2
```

3\. Create VM 2 with 16 vCPU and 32GB DIMM on 1 channel:
```bash
./admin/ramryder_cli create-vm \
  --cpu-set 8-15,136-143 \
  --memory 32G \
  --channels 1 \
  --image /path/to/nvcloud-image-vm2.qcow2
```

4\. Create VM 3 with 48 vCPU and 96GB DIMM on 3 channel:
```bash
./admin/ramryder_cli create-vm \
  --cpu-set 16-39,144-167 \
  --memory 96G \
  --channels 3 \
  --image /path/to/nvcloud-image-vm3.qcow2
```

4\. Create VM 4 with 48 vCPU and 96GB DIMM on 3 channel:
```bash
./admin/ramryder_cli create-vm \
  --cpu-set 40-63,168-191 \
  --memory 96G \
  --channels 3 \
  --image /path/to/nvcloud-image-vm4.qcow2
```
## Benchmark VMs
After VMs are ready, log in to each VM and run target workloads. RamRyder project maintains a [benchmarks suit](https://github.com/ramryder-project/mem-benchmarks) which include all memory related benchmarks.  

1\. Get Benchmarks
```bash
git clone --recurse-submodules git@github.com:ramryder-project/mem-benchmarks.git
```

2\. Run Microbenchmark MLC
```bash
cd mem-benchmarks
./mlc/mlc --loaded_latency
```

3\. Run STREAM
```bash
./scripts/STREAM/compile_stream.sh
./STREAM/stream_c.exe
```

4\. Run YCSB
YCSB supports multiples databases, each involves unique configuration and setup. Please refer to [YCSB website](https://github.com/brianfrankcooper/YCSB/tree/master) to set up target databases (e.g., [Redis](https://github.com/brianfrankcooper/YCSB/tree/master/redis) and [Memcached](https://github.com/brianfrankcooper/YCSB/tree/master/memcached)) and run YCSB workloads. 

RamRyder provides an example script to run YCSB workloads on Memcached.
```bash
./scripts/memcached/load_ycsb_a.sh
./scripts/memcached/run_ycsb_a.sh
./scripts/memcached/run_ycsb_b.sh
```
# Documentation

[Documentation](https://ramryder-project.github.io/docs/) provdes complete instrudctions. For more detailed installation steps, configuration guidance, and module-level explanations, please refer to the documentation:

- [DIMM Setup](https://ramryder-project.github.io/docs/hardware-support/dimm)
- [PMem Setup](https://ramryder-project.github.io/docs/hardware-support/pmem)
- [CXL Setup](https://ramryder-project.github.io/docs/hardware-support/cxl)
- [Resource Manager Setup](https://ramryder-project.github.io/docs/getting-started/resource-manager-setup)
- [Virtual Machine Setup](https://ramryder-project.github.io/docs/getting-started/virtual-machine-setup)
- [Kernel Installation](https://ramryder-project.github.io/docs/getting-started/kernel-installation)
