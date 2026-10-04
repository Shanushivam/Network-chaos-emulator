# Chaos Emulator

A Linux kernel module that injects network faults (**packet loss**, **packet corruption** and **latency**) into live IPv4 traffic, controlled at runtime by an interactive C++ program.

> **Warning:** This module touches every IPv4 packet on the host. Only load it on a disposable VM or test machine, never on a system you depend on or reach only over SSH. See [Known limitations](#known-limitations).

---

## Table of contents

- [Why this project](#why-this-project)
- [Features](#features)
- [Architecture](#architecture)
- [How a packet is processed](#how-a-packet-is-processed)
- [Kernel ↔ userspace interface](#kernel--userspace-interface)
- [Project structure](#project-structure)
- [Requirements](#requirements)
- [Build](#build)
- [Usage](#usage)
- [Example session](#example-session)
- [Known limitations](#known-limitations)
- [Future work](#future-work)
- [What I learned](#what-i-learned)
- [License](#license)

---

## Why this project

Real networks are unreliable: packets get lost, arrive late or arrive damaged. Software that only ever runs on a fast, clean local network can hide bugs in timeouts, retries and error handling. **Chaos testing** deliberately injects these faults to see how applications behave under bad conditions.

Linux already provides a production tool for this (`tc qdisc ... netem`). This project builds a simplified version from scratch, **inside the kernel**, to learn how the pieces work:

- intercepting packets with **netfilter hooks**
- exposing a **character device** from a kernel module
- passing structured data between userspace and the kernel with **`ioctl`**
- keeping lock-free counters with **atomic operations**

## Features

| Fault           | What it does                                               |
|-----------------|------------------------------------------------------------|
| Packet loss     | Drops a configurable percentage of packets (0–100%)        |
| Corruption      | Flips one byte in a configurable percentage of packets     |
| Latency         | Delays each packet by a configurable time (max 50 ms)      |
| Direction       | Ingress and egress can be enabled independently            |
| Live statistics | Total / dropped / delayed / corrupted packet counters      |

Rules can be changed or cleared at any time without reloading the module.

## Architecture

```
            USERSPACE                                KERNEL SPACE
 ┌───────────────────────────┐          ┌──────────────────────────────────────┐
 │      chaos_control        │          │          chaos_driver.ko             │
 │      (main.cpp)           │          │                                      │
 │                           │  ioctl   │  ┌────────────────────────────────┐  │
 │  1. Apply rules  ─────────┼──────────┼─►│ CHAOS_SET_CONFIG               │  │
 │  2. Clear rules  ─────────┼──────────┼─►│   → current_config             │  │
 │  3. Fetch stats  ◄────────┼──────────┼──│ CHAOS_GET_STATS                │  │
 │                           │          │  │   ← atomic64 counters          │  │
 └───────────────────────────┘          │  └───────────────┬────────────────┘  │
              │                         │                  │ read by           │
              ▼                         │                  ▼                   │
     /dev/chaos_emulator  ──────────────┼─►  ┌──────────────────────────────┐  │
     (character device)                 │    │       process_packet()       │  │
                                        │    │  drop → corrupt → delay      │  │
                                        │    └──────▲───────────────▲───────┘  │
                                        │           │               │          │
                                        │  hook_ingress()     hook_egress()    │
                                        └───────────┼───────────────┼──────────┘
                                                    │               │
    Network ──► NIC ──► [ PRE_ROUTING ] ──► routing ──► ... ──► [ POST_ROUTING ] ──► NIC ──► Network
                         (incoming IPv4)                         (outgoing IPv4)
```

**Components**

1. **Character device (`/dev/chaos_emulator`)**: created at module load with `alloc_chrdev_region`, `cdev_add`, `class_create` and `device_create`. It is the control channel between userspace and the module.
2. **Netfilter hooks**: two hooks registered with `nf_register_net_hook`, both at priority `NF_IP_PRI_FIRST` so they run before other netfilter rules (such as iptables):
   - `NF_INET_PRE_ROUTING`: every incoming IPv4 packet, before the routing decision.
   - `NF_INET_POST_ROUTING`: every outgoing IPv4 packet, just before it leaves.
3. **Shared state**: a global `chaos_config` struct holds the active rules. Four `atomic64_t` counters track statistics, so packets on many CPUs can update them without locks.
4. **Control tool (`chaos_control`)**: opens the device, reads user input and issues `ioctl` calls.

## How a packet is processed

Every intercepted packet goes through `process_packet()` in this order:

```
packet arrives at hook
        │
        ▼
direction enabled? ── no ──► NF_ACCEPT (untouched)
        │ yes
        ▼
total++ 
        │
        ▼
random % 100 < loss_rate? ── yes ──► dropped++ ──► NF_DROP (packet discarded)
        │ no
        ▼
random % 100 < corrupt_rate? ── yes ──► make skb writable, flip a byte ──► corrupted++
        │
        ▼
latency_ms > 0? ── yes ──► delayed++ ──► mdelay(min(latency_ms, 50))
        │
        ▼
NF_ACCEPT (packet continues)
```

Notes:

- Randomness comes from the kernel's `get_random_bytes()`.
- Faults are checked in order and dropped packets exit early, so a dropped packet is never also corrupted or delayed.
- With both directions enabled, a round trip (for example a ping) passes **two** hooks on this machine: egress for the request and ingress for the reply. The effective loss and latency are therefore roughly **doubled** for round-trip traffic.
- Counters are shared across both directions.

## Kernel ↔ userspace interface

Both sides define the same structs and ioctl numbers (magic `'C'`):

```c
#define CHAOS_SET_CONFIG _IOW('C', 1, struct chaos_config)
#define CHAOS_GET_STATS  _IOR('C', 2, struct chaos_stats)

struct chaos_config {
    int latency_ms;      // delay per packet in ms (capped at 50)
    int loss_rate;       // % of packets to drop (0–100)
    int corrupt_rate;    // % of packets to corrupt (0–100)
    int enable_ingress;  // 1 = apply to incoming traffic
    int enable_egress;   // 1 = apply to outgoing traffic
};

struct chaos_stats {
    unsigned long total_packets;
    unsigned long dropped_packets;
    unsigned long delayed_packets;
    unsigned long corrupted_packets;
};
```

- `CHAOS_SET_CONFIG` copies a config in from userspace with `copy_from_user` and replaces the active rules.
- `CHAOS_GET_STATS` reads the atomic counters into a struct and copies it out with `copy_to_user`.
- Unknown commands return `-ENOTTY`.

Default config at load time: `{0, 0, 0, 1, 1}` (no faults, both directions enabled).

## Project structure

```
chaos_emulator/
├── chaos_driver.c   # Kernel module: char device, ioctl handler, netfilter hooks
├── main.cpp         # Userspace control center (chaos_control)
├── Makefile         # Kbuild makefile for the kernel module
└── README.md
```

Running `make` also produces build artifacts (`*.o`, `*.ko`, `*.mod*`, `.*.cmd`, `Module.symvers`, `modules.order`). These are generated files and are not part of the source.

## Requirements

- Linux with headers for the running kernel (`linux-headers-$(uname -r)`)
- `make`, `gcc`, `g++`
- Root access to load the module

Tested on kernel 7.0 (arm64). Kernels older than 6.4 need the two-argument form `class_create(THIS_MODULE, CLASS_NAME)`.

On Debian/Ubuntu:

```bash
sudo apt install build-essential linux-headers-$(uname -r)
```

## Build

Build the kernel module:

```bash
make
```

Build the control tool:

```bash
g++ -O2 -o chaos_control main.cpp
```

## Usage

Load the module:

```bash
sudo insmod chaos_driver.ko
```

Check that it loaded and created the device:

```bash
lsmod | grep chaos_driver
```

```bash
ls -l /dev/chaos_emulator
```

Allow the control tool to open the device (see the security note in [Known limitations](#known-limitations)):

```bash
sudo chmod 666 /dev/chaos_emulator
```

Run the control center:

```bash
./chaos_control
```

Menu options:

| Option | Action                                                        |
|--------|---------------------------------------------------------------|
| 1      | Apply rules: enter latency (ms), loss (%) and corruption (%)  |
| 2      | Clear all rules: restore normal networking                    |
| 3      | Fetch live kernel stats                                       |
| 4      | Exit (rules stay active until cleared or the module unloads)  |

Watch the effect from a second terminal:

```bash
ping -c 20 8.8.8.8
```

Unload the module when finished. This removes the hooks and restores normal networking:

```bash
sudo rmmod chaos_driver
```

Remove build artifacts:

```bash
make clean
```

## Example session

Applying 30% loss and 20 ms latency, then reading stats:

```
====================================
    NETWORK CHAOS CONTROL CENTER    
====================================
1. Apply Chaos Rules (Latency, Loss, Corruption)
2. Clear All Rules (Normal Network)
3. Fetch Live Kernel Stats
4. Exit
Select Option [1-4]: 1
--> Enter Latency (ms): 20
--> Enter Packet Loss Rate (0-100%): 30
--> Enter Corruption Rate (0-100%): 0

[SUCCESS] Chaos Rules Sent to Kernel Module!

Select Option [1-4]: 3

--- LIVE KERNEL TELEMETRY ---
Total Packets Intercepted : <N>
Packets Dropped           : <N>
Packets Delayed           : <N>
Packets Corrupted         : <N>
```

<!-- TODO: replace <N> with real numbers and add before/after `ping` output from a test run. -->

Expected `ping` behavior with these settings:

- **Before:** 0% packet loss, normal round-trip time.
- **After:** noticeable packet loss (higher than 30%, since each round trip passes two hooks), and round-trip time increased by about 40 ms (20 ms on egress + 20 ms on ingress).

## Known limitations

This is a learning project, and it has real issues:

- **Latency is a busy-wait.** `mdelay()` runs inside the netfilter hook (often in softirq context) and spins the CPU for every delayed packet. Under load this can stall the system or trigger soft-lockup / RCU-stall warnings. That is why latency is capped at 50 ms.
- **Corruption hits the IP header, not the payload.** The code flips byte 19 of the network header, which is the last byte of the **destination IP address**, and does not update the checksum. Outgoing corrupted packets are discarded by the next hop. Incoming ones have already passed checksum validation and get misrouted.
- **No permission check.** The ioctl handler does not check `CAP_NET_ADMIN`, and the setup uses `chmod 666`, so any local user can change the rules.
- **No input validation.** Rates above 100 or negative values are accepted. Latency above 50 ms is silently capped.
- **Non-atomic config updates.** The config struct is replaced while other CPUs may be reading it, so a packet can briefly see a mix of old and new values.
- **Unchecked hook registration.** Return values of `nf_register_net_hook` are ignored.
- **IPv4 only, initial network namespace only.** IPv6 traffic and traffic inside containers (separate network namespaces) are not affected.
- **Duplicated definitions.** The structs and ioctl numbers are copied in both files instead of shared through a header. There is no `compat_ioctl` for 32-bit userspace on a 64-bit kernel.
- **Unvalidated CLI input.** Non-numeric input in `chaos_control` leaves fields uninitialized.

## Future work

- [ ] Replace `mdelay()` with real queuing: hold packets (`NF_QUEUE` or a private queue) and reinject them with an `hrtimer`.
- [ ] Corrupt the transport payload (after `ip_hdrlen()` + transport header) instead of the IP header.
- [ ] Require `CAP_NET_ADMIN` in the ioctl handler and add a udev rule instead of `chmod 666`.
- [ ] Validate config ranges and return `-EINVAL` on bad input.
- [ ] Protect the config with RCU or a spinlock.
- [ ] Check hook registration results and clean up properly on failure.
- [ ] Add IPv6 hooks and per-namespace support.
- [ ] Add per-direction counters, jitter, duplication and reordering.
- [ ] Filter by IP, port or protocol instead of affecting all traffic.
- [ ] Move shared definitions to a common header (`chaos_ioctl.h`).
- [ ] Add command-line flags to `chaos_control` for scripting (for example `--loss 10 --latency 20`).

## What I learned

- How netfilter exposes hook points in the IPv4 stack and how verdicts (`NF_ACCEPT`, `NF_DROP`) control a packet's fate.
- How to register a character device and expose an `ioctl` interface from a kernel module.
- Safely moving data across the user/kernel boundary with `copy_from_user` / `copy_to_user`.
- Why sleeping or busy-waiting in packet-processing context is dangerous, and why production tools like `netem` queue packets instead.
- The structure of `sk_buff` and the need to make it writable (`skb_try_make_writable`) before modifying packet data.
- Using atomic counters for statistics updated concurrently from many CPUs.

## License

GPL-2.0, as declared by `MODULE_LICENSE("GPL")` in the module.
