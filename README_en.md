> This project is not affiliated with or endorsed by MikroTik / SIA Mikrotikls

# awg-proxy -- AmneziaWG for MikroTik

[![C11](https://img.shields.io/badge/C-11-blue)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[Русская версия](README.md) | [GitHub](https://github.com/timbrs/amneziawg-mikrotik-c)

Lightweight Docker container that allows MikroTik routers to connect to AmneziaWG servers. All traffic is encrypted by the router's native WireGuard client; the container only transforms the packet format.

## Table of Contents

- [How It Works](#how-it-works)
- [Quick Start (Configurator)](#quick-start-configurator)
- [Requirements](#requirements)
- [Manual Installation](#manual-installation)
- [Getting AWG Parameters](#getting-awg-parameters)
- [Additional Settings](#additional-settings)
- [Uninstallation](#uninstallation)
- [Troubleshooting](#troubleshooting)
  - [execvpe /awg-proxy: No such file or directory](#execvpe-awg-proxy-no-such-file-or-directory)
  - [Site-to-site: handshake did not complete](#site-to-site-handshake-did-not-complete)
  - [Storage device not found](#storage-device-not-found)
  - [Insufficient disk space](#insufficient-disk-space)
  - [not allowed by device-mode](#not-allowed-by-device-mode)
  - [child spawn failed / could not load next layer](#child-spawn-failed--could-not-load-next-layer)
- [Building from Source](#building-from-source)
- [License](#license)

## How It Works

### Normal Mode (default)

```
MikroTik WG client ──UDP──> [awg-proxy] ──UDP──> AmneziaWG server
   (encryption)          (transformation)          (obfuscation)
```

The proxy replaces packet headers, adds padding and junk packets so the AmneziaWG server accepts the traffic. Keys and data are not modified.

### Reverse Mode (site-to-site)

```
MikroTik1 WG ↔ [proxy1 normal] ──AWG──> [proxy2 reverse] ↔ MikroTik2 WG
```

Reverse proxy: accepts AWG traffic from a normal proxy, transforms it back to standard WireGuard, and forwards to a local WG server. Allows connecting two MikroTik routers via AWG without running a separate AWG server.

### Server Mode (hub, 1:N)

```
proxy1a (normal) ──AWG──┐
proxy1b (normal) ──AWG──┤──> [reverse-hub] ──WG──> WG server
proxy1c (normal) ──AWG──┘
```

Multi-client reverse proxy: accepts AWG connections from multiple normal proxies and routes responses from the WG server to the correct client via a built-in session table. Each peer uses ~16 bytes in the hash table.

Compatible with AWG v1 and v2 -- the version is detected automatically based on the environment variables.

## Quick Start (Configurator)

1. Export a `.conf` file from AmneziaVPN (see [Getting AWG Parameters](#getting-awg-parameters))
2. Open the [configurator](https://timbrs.github.io/amneziawg-mikrotik-c/configurator.html)
3. Paste the `.conf` file contents
4. Copy the generated commands and run them in MikroTik terminal

Done. The configurator works offline; no data is sent to any server.

![Speed test on MikroTik AX3](https://github.com/user-attachments/assets/9fb34444-681b-4f34-8306-8f202f1b121d)

*Speed test on MikroTik AX3*

## Requirements

- An AmneziaWG server with known obfuscation parameters
- Configuration file `.conf` exported from AmneziaVPN
- MikroTik RouterOS 7.4+ with the **container** package
  - **RouterOS 7.21+**: standard images `awg-proxy-{arch}.tar.gz` (OCI format)
  - **RouterOS 7.20 and below**: images `awg-proxy-{arch}-7.20-Docker.tar.gz` (Docker format)
  - The configurator detects the version automatically
- Architecture: ARM64, ARM (v7), or x86_64 ([check your device](https://help.mikrotik.com/docs/spaces/ROS/pages/84901929/Container))
- At least 5 MB free disk space (or a USB drive)
- At least 16 MB free RAM

## Manual Installation

### 1. Enable Containers and Fetch

Install the container package from [mikrotik.com](https://mikrotik.com/download), upload it to the router, and reboot. Then:

```routeros
/system/device-mode/update container=yes fetch=yes
```

`fetch=yes` is required to download the image directly on the router via `/tool/fetch`. If you plan to upload the file manually via Winbox/SCP, `fetch=yes` is not required.

The router will ask for confirmation (button press or reboot, depending on the model).

### 2. Upload Image

Download `awg-proxy-{arch}.tar.gz` from [Releases](https://github.com/timbrs/amneziawg-mikrotik-c/releases) and upload it to the router via Winbox or SCP. For RouterOS 7.20 and below, use files with the `-7.20-Docker` suffix (Docker format).

Or download directly on the router (replace URL with the actual one):

```routeros
/tool/fetch url="https://github.com/timbrs/amneziawg-mikrotik-c/releases/latest/download/awg-proxy-arm64.tar.gz" dst-path=awg-proxy-arm64.tar.gz
```

### 3. Network Setup

```routeros
/interface/veth/add name=veth-awg-proxy address=172.18.0.2/30 gateway=172.18.0.1
/ip/address/add address=172.18.0.1/30 interface=veth-awg-proxy
/ip/firewall/nat/add chain=srcnat action=masquerade src-address=172.18.0.0/30
```

### 4. WireGuard

```routeros
/interface/wireguard/add name=wg-awg-proxy private-key="YOUR_PRIVATE_KEY" listen-port=12429
/interface/wireguard/peers/add interface=wg-awg-proxy public-key="SERVER_PUBLIC_KEY" \
    preshared-key="YOUR_PRESHARED_KEY" endpoint-address=172.18.0.2 endpoint-port=51820 \
    allowed-address=0.0.0.0/0 persistent-keepalive=25
/ip/address/add address=YOUR_TUNNEL_IP interface=wg-awg-proxy
```

Replace:
- `YOUR_PRIVATE_KEY` -- PrivateKey from `[Interface]`
- `SERVER_PUBLIC_KEY` -- PublicKey from `[Peer]`
- `YOUR_PRESHARED_KEY` -- PresharedKey from `[Peer]` (if any)
- `YOUR_TUNNEL_IP` -- Address from `[Interface]` (e.g., `10.8.0.2/32`)

### 5. Environment Variables

```routeros
/container/envs/add list=awg-proxy-env key=AWG_LISTEN value=":51820"
/container/envs/add list=awg-proxy-env key=AWG_REMOTE value="SERVER_IP:PORT"
/container/envs/add list=awg-proxy-env key=AWG_JC value="5"
/container/envs/add list=awg-proxy-env key=AWG_JMIN value="30"
/container/envs/add list=awg-proxy-env key=AWG_JMAX value="500"
/container/envs/add list=awg-proxy-env key=AWG_S1 value="20"
/container/envs/add list=awg-proxy-env key=AWG_S2 value="20"
/container/envs/add list=awg-proxy-env key=AWG_H1 value="1234567890"
/container/envs/add list=awg-proxy-env key=AWG_H2 value="1234567891"
/container/envs/add list=awg-proxy-env key=AWG_H3 value="1234567892"
/container/envs/add list=awg-proxy-env key=AWG_H4 value="1234567893"
/container/envs/add list=awg-proxy-env key=AWG_SERVER_PUB value="SERVER_PUBLIC_KEY"
/container/envs/add list=awg-proxy-env key=AWG_CLIENT_PUB value=[/interface/wireguard/get [find name=wg-awg-proxy] public-key]
```

Replace all values with parameters from your `.conf` file. `AWG_CLIENT_PUB` is read automatically from the WireGuard interface.

### 6. Create and Start Container

```routeros
/container/add file=awg-proxy-arm64.tar.gz interface=veth-awg-proxy envlist=awg-proxy-env \
    hostname=awg-proxy root-dir=disk1/awg-proxy logging=yes shm-size=4M start-on-boot=yes
/container/start [find where tag~"awg-proxy"]
```

Verify it works:

```routeros
/container/print
/interface/wireguard/peers/print
```

The container should show `running` status, and the peer should have a `last-handshake` value.

## Getting AWG Parameters

1. Open the **AmneziaVPN** application
2. Select the desired connection
3. Tap **Share**
4. Choose: **Protocol**: AmneziaWG, **Format**: AmneziaWG Format
5. Save the `.conf` file

The obfuscation parameters (`Jc`, `Jmin`, `Jmax`, `S1`, `S2`, `H1`--`H4`) are in the `[Interface]` section, while `Endpoint` and `PublicKey` are in the `[Peer]` section.

## Additional Settings

### All Environment Variables

| Variable | Required | Default | Description |
|----------|:---:|:---:|-------------|
| `AWG_LISTEN` | Yes | -- | Listen address |
| `AWG_REMOTE` | Yes | -- | AWG server address |
| `AWG_JC` | Yes | -- | Junk packet count |
| `AWG_JMIN` | Yes | -- | Min junk packet size |
| `AWG_JMAX` | Yes | -- | Max junk packet size |
| `AWG_S1` | Yes | -- | Handshake init padding |
| `AWG_S2` | Yes | -- | Handshake response padding |
| `AWG_H1`--`AWG_H4` | Yes | -- | Message types |
| `AWG_SERVER_PUB` | Yes | -- | Server public key |
| `AWG_CLIENT_PUB` | Yes | -- | Client public key |
| `AWG_S3` | No | `0` | Cookie reply padding (v2) |
| `AWG_S4` | No | `0` | Transport data padding (v2) |
| `AWG_I1`--`AWG_I5` | No | -- | CPS templates (v1.5/v2) |
| `AWG_MODE` | No | `normal` | Operating mode: `normal`, `reverse`, `server` |
| `AWG_SRC_PORT` | No | auto | Outgoing port to server |
| `AWG_TIMEOUT` | No | `180` | Inactivity timeout (sec) |
| `AWG_LOG_LEVEL` | No | `info` | Log level |
| `AWG_NO_GRO` | No | `0` | Disable UDP GRO |
| `AWG_SOCKET_BUF` | No | `16777216` | Socket buffer size |
| `AWG_CPU_C2S` | No | `-1` | CPU for client→server thread |
| `AWG_CPU_S2C` | No | `-1` | CPU for server→client thread |
| `AWG_BUSY_POLL` | No | `0` | SO_BUSY_POLL timeout (μs) |
| `AWG_DNS` | No | -- | DNS server for hostname resolution in AWG_REMOTE |

The protocol version is detected automatically: **v2** if S3/S4 are set or H values are ranges, **v1.5** if CPS templates (I1-I5) are set, otherwise **v1**.

### Detailed Variable Descriptions

#### Required -- Obfuscation Parameters

All values are taken from the `.conf` file exported from AmneziaVPN (`[Interface]` and `[Peer]` sections). They must **exactly** match the server parameters, otherwise the handshake will fail.

**`AWG_LISTEN`** -- address and port where the proxy listens for UDP packets from the router's WireGuard client. Format: `address:port` or `:port` (listen on all interfaces).

```
AWG_LISTEN=:51820          # all interfaces, port 51820 (standard)
AWG_LISTEN=172.18.0.2:9000 # specific address and port
```

**`AWG_REMOTE`** -- address and port of the AWG server (`Endpoint` from `[Peer]`). Supports both IP addresses and domain names.

```
AWG_REMOTE=1.2.3.4:443            # IP + port
AWG_REMOTE=vpn.example.com:51820  # domain + port
```

**`AWG_JC`**, **`AWG_JMIN`**, **`AWG_JMAX`** -- junk packet parameters. Before each handshake init, `JC` random UDP packets of size between `JMIN` and `JMAX` bytes are sent. The server discards them, but they look like regular traffic to DPI. Values from `.conf` (`Jc`, `Jmin`, `Jmax`).

```
AWG_JC=5      # 5 junk packets before handshake
AWG_JMIN=30   # minimum 30 bytes
AWG_JMAX=500  # maximum 500 bytes

AWG_JC=0      # junk packets disabled
```

**`AWG_S1`**, **`AWG_S2`** -- number of padding bytes added to handshake init (S1) and handshake response (S2). Changes packet sizes so DPI cannot identify WireGuard handshake by its characteristic sizes of 148 and 92 bytes. Values from `.conf` (`S1`, `S2`).

```
AWG_S1=20   # +20 bytes to handshake init (148 → 168)
AWG_S2=20   # +20 bytes to handshake response (92 → 112)

AWG_S1=0    # padding disabled
AWG_S2=0
```

**`AWG_H1`**, **`AWG_H2`**, **`AWG_H3`**, **`AWG_H4`** -- WireGuard message type substitution. Standard types (1, 2, 3, 4) are replaced with the specified values so DPI cannot recognize the protocol. In v1 -- fixed numbers, in v2 -- can be `min-max` ranges. Values from `.conf` (`H1`--`H4`).

```
# v1: fixed values
AWG_H1=1234567890
AWG_H2=1234567891
AWG_H3=1234567892
AWG_H4=1234567893

# v2: ranges (random value from range for each packet)
AWG_H1=100-200
AWG_H4=1000-2000
```

**`AWG_SERVER_PUB`**, **`AWG_CLIENT_PUB`** -- server and client public keys in base64 format (44 characters). Used for MAC1 recalculation in handshake packets after header substitution. Without correct keys, the MAC check on the server will fail.

```
AWG_SERVER_PUB=kB3VpJIEGVTW2D4GR0cC/c3bOEG3jNIm5MjHJkSIj2I=
AWG_CLIENT_PUB=aBcDeFgHiJkLmNoPqRsTuVwXyZ0123456789+/ABCD=

# Automatic retrieval from the router's WireGuard interface:
AWG_CLIENT_PUB=[/interface/wireguard/get [find name=wg-awg-proxy] public-key]
```

#### Optional -- Protocol v2

**`AWG_S3`**, **`AWG_S4`** -- padding for cookie reply (S3) and transport data (S4). Introduced in AWG v2. If S3 > 0 or S4 > 0 are set, the proxy automatically switches to v2 mode.

```
AWG_S3=0    # default, no padding
AWG_S4=16   # +16 bytes to each transport data packet
```

**`AWG_I1`--`AWG_I5`** -- CPS templates (Constant Packet Size). Up to 5 templates for generating fixed-format packets before handshake. If set without S3/S4/H ranges, the proxy operates in v1.5 mode. Template format is described in the AWG documentation.

```
AWG_I1=b:48656c6c6f,r:10,t:4,c:4
```

#### Optional -- Operating Mode

**`AWG_MODE`** -- proxy operating mode. Determines the direction of packet transformation.

- `normal` (default) -- standard proxy: accepts WireGuard from the router, transforms to AWG, and sends to the AWG server.
- `reverse` -- reverse proxy (1:1 site-to-site): accepts AWG from another normal proxy, transforms back to WireGuard, and sends to a local WG server. Used in pair with a normal proxy on the other side.
- `server` -- reverse proxy hub (1:N): like reverse, but supports connections from multiple normal proxies simultaneously. Response routing from the WG server to the correct client is done via a session table using `sender_index`/`receiver_index` from WireGuard packets.

```
AWG_MODE=normal    # default
AWG_MODE=reverse   # reverse proxy, 1:1
AWG_MODE=server    # reverse proxy hub, 1:N
```

In `reverse` and `server` modes, `AWG_REMOTE` points to the WireGuard server (not the AWG server), while `AWG_LISTEN` accepts AWG traffic from normal proxies. Obfuscation parameters (H1--H4, S1--S4, JC, etc.) must match the normal proxy on the other side.

#### Optional -- Network and Diagnostics

**`AWG_SRC_PORT`** -- outgoing UDP port for the connection to the AWG server. By default (`auto`), the proxy uses the WireGuard client's port -- this is needed for correct NAT operation on the router. If a number is specified, a fixed port is used.

```
AWG_SRC_PORT=auto    # default, copies the WG client port
AWG_SRC_PORT=0       # same as auto
AWG_SRC_PORT=12345   # fixed port 12345
```

**`AWG_TIMEOUT`** -- inactivity timeout in seconds. If no packets are sent or received in either direction within this time, the proxy reconnects to the server (re-resolves DNS + new socket). Useful when the server's IP address changes behind DNS.

```
AWG_TIMEOUT=180   # default, 3 minutes
AWG_TIMEOUT=60    # aggressive timeout for unstable connections
AWG_TIMEOUT=3600  # 1 hour, for stable links
```

**`AWG_LOG_LEVEL`** -- logging level. Controls the verbosity of output in `/container/print` and the router's syslog.

- `none` -- no output (for production on low-power devices)
- `error` -- errors only (bind/connect failed, reconnect)
- `info` -- startup config, client connections, reconnects (default)
- `debug` -- packet tracing: handshake init, junk sending, GRO segments, send errors. Needed for diagnosing handshake problems

```
AWG_LOG_LEVEL=info    # default
AWG_LOG_LEVEL=debug   # full tracing for debugging
AWG_LOG_LEVEL=error   # errors only
AWG_LOG_LEVEL=none    # silence
```

**`AWG_NO_GRO`** -- disables UDP GRO (Generic Receive Offload) on the server socket. GRO coalesces multiple incoming UDP packets into a single buffer, reducing the number of system calls. Enabled by default if the kernel supports it. On some platforms (ARM64 on RouterOS) the kernel accepts the setsockopt call, but GRO doesn't actually work -- in this case the proxy hangs waiting for packets. Set `AWG_NO_GRO=1` to force disable.

```
AWG_NO_GRO=0   # default, GRO enabled (if kernel supports it)
AWG_NO_GRO=1   # force disable GRO, use recvmmsg instead
```

**`AWG_SOCKET_BUF`** -- receive/send buffer sizes (SO_RCVBUF/SO_SNDBUF) for UDP sockets in bytes. The kernel typically doubles the requested value. Larger buffers reduce packet loss under load but consume more RAM.

```
AWG_SOCKET_BUF=16777216  # default, 16 MB
AWG_SOCKET_BUF=4194304   # 4 MB, for memory-constrained devices
AWG_SOCKET_BUF=1048576   # 1 MB, minimum recommended
```

#### Optional -- Performance

These parameters are only relevant on powerful devices with multiple CPU cores. On typical MikroTik routers (1-2 cores), leave the defaults.

**`AWG_CPU_C2S`**, **`AWG_CPU_S2C`** -- thread-to-CPU pinning (CPU affinity). The proxy uses two threads: c2s (client→server, outbound packet processing) and s2c (server→client, inbound). Pinning to different cores prevents thread migration and improves cache efficiency.

```
AWG_CPU_C2S=-1   # default, OS chooses the core
AWG_CPU_S2C=-1

AWG_CPU_C2S=0    # c2s on core 0
AWG_CPU_S2C=1    # s2c on core 1
```

**`AWG_BUSY_POLL`** -- enables SO_BUSY_POLL on sockets. The kernel actively polls the network driver for the specified time (in microseconds) instead of going to sleep. Reduces latency by ~50 μs but increases CPU usage. Requires network driver support.

```
AWG_BUSY_POLL=0     # default, disabled
AWG_BUSY_POLL=50    # 50 μs of active polling
AWG_BUSY_POLL=100   # 100 μs, for minimum latency
```

### Routing Traffic Through the Tunnel

Specific host:

```routeros
/ip/route/add dst-address=8.8.8.8/32 gateway=wg-awg-proxy
```

Subnet:

```routeros
/ip/route/add dst-address=10.0.0.0/8 gateway=wg-awg-proxy
```

View routes:

```routeros
/ip/route/print where gateway=wg-awg-proxy
```

Remove a route:

```routeros
/ip/route/remove [find where dst-address="8.8.8.8/32" gateway="wg-awg-proxy"]
```

### DNS Through the Tunnel

To route DNS queries through the tunnel, set DNS servers and add routes to them:

```routeros
/ip/dns/set servers=8.8.8.8,8.8.4.4
/ip/route/add dst-address=8.8.8.8/32 gateway=wg-awg-proxy
/ip/route/add dst-address=8.8.4.4/32 gateway=wg-awg-proxy
```

### Address-List Based Routing (Advanced)

For selective traffic routing through the tunnel, use routing tables and mangle rules.

Create a routing table:

```routeros
/routing/table/add disabled=no fib name=r_to_vpn
```

Default route through the tunnel for this table:

```routeros
/ip/route/add dst-address=0.0.0.0/0 gateway=wg-awg-proxy routing-table=r_to_vpn
```

Address list with destinations to route through the tunnel:

```routeros
/ip/firewall/address-list/add address=8.8.8.8 list=to_vpn
/ip/firewall/address-list/add address=1.1.1.1 list=to_vpn
```

Mangle rules for traffic marking:

```routeros
# Skip local traffic
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=10.0.0.0/8
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=172.16.0.0/12
/ip/firewall/mangle/add chain=prerouting action=accept dst-address=192.168.0.0/16

# Mark connections to addresses in the list
/ip/firewall/mangle/add chain=prerouting action=mark-connection \
    dst-address-list=to_vpn connection-mark=no-mark \
    new-connection-mark=to-vpn-conn passthrough=yes

# Mark routing for tagged connections
/ip/firewall/mangle/add chain=prerouting action=mark-routing \
    connection-mark=to-vpn-conn new-routing-mark=r_to_vpn passthrough=yes
```

NAT for marked traffic:

```routeros
/ip/firewall/nat/add chain=srcnat action=masquerade routing-mark=r_to_vpn
```

Now all traffic to addresses in the `to_vpn` list will go through the tunnel. Add addresses to the list as needed.

## Uninstallation

If installed via the configurator:

```routeros
/system/script/run awg-proxy-uninstall
```

The script removes the container, WireGuard interface, NAT rules, routes, environment variables, restores DNS settings, and deletes itself.

## Troubleshooting

**Container does not start** -- check the container package is installed (`/system/package/print`), device mode is enabled (`/system/device-mode/print`), and there is enough disk space (`/system/resource/print`).

### execvpe /awg-proxy: No such file or directory

The container starts but immediately exits with `exited with status 255: execvpe /awg-proxy: No such file or directory`. This means the binary was not extracted — the image was downloaded incorrectly or incompletely.

1. Remove the container and root-dir:
```routeros
/container/stop [find where comment=awg-proxy]
:delay 7s
/container/remove [find where comment=awg-proxy]
/file/remove disk1/awg-proxy
:do { /file/remove [find where name~"awg-proxy.*tar"] } on-error={}
```

2. Re-download the image and verify the file size (`/file/print`) — it should be 100-300 KB, not 0.

3. Re-create the container.

### Site-to-site: handshake did not complete

In site-to-site mode (two MikroTik routers via AWG proxy), the handshake does not complete even though both containers are running. Common causes:

**1. Firewall forward chain on Side B (server)**

DSTNAT traffic goes through the `forward` chain, not `input`. If the `accept` rule is appended at the end and there's a `drop` rule above it, packets never reach the container.

Diagnosis:
```routeros
/ip/firewall/filter/print where chain=forward
```

Fix — move the rule to the top:
```routeros
/ip/firewall/filter/remove [find where comment=PREFIX-awg-in]
/ip/firewall/filter/add chain=forward action=accept protocol=udp dst-port=AWG_PORT in-interface-list=WAN place-before=0 comment=PREFIX-awg-in
```

**2. Firewall input chain on Side B (server)**

In reverse mode, the container initiates a NEW connection to MikroTik's WG port (unlike standard mode where MikroTik initiates and the container's response is "established"). If the veth interface is not in the LAN interface list, the input chain drops packets from the container.

Fix:
```routeros
/ip/firewall/filter/add chain=input action=accept protocol=udp src-address=CONTAINER_IP dst-port=WG_PORT place-before=0 comment=PREFIX-wg-in
```

**3. DNS resolution of AWG_REMOTE on Side A (client)**

If `AWG_REMOTE` is a hostname, the container needs working DNS. Set `AWG_DNS=8.8.8.8` or `AWG_DNS=1.1.1.1` in the container environment variables. If DNS also goes through the tunnel (circular dependency), resolve the hostname manually and use the IP:
```routeros
:put [:resolve vpn.example.com]
# Then set the resolved IP in AWG_REMOTE
```

**4. Diagnostics via logs**

Enable debug logging on both containers:
```routeros
/container/envs/add list=PREFIX-env key=AWG_LOG_LEVEL value=debug
```
Restart the containers and check logs — they will show DNS resolution errors, connect failures, handshake init and junk packet sending.

**No handshake** -- make sure all AWG parameters (Jc, Jmin, Jmax, S1, S2, H1--H4) exactly match the server. Verify `AWG_REMOTE`, `AWG_SERVER_PUB`, and `AWG_CLIENT_PUB`. For diagnostics, set `AWG_LOG_LEVEL=debug` -- logs will show handshake init and junk packet sending. If you see `remote read error (Connection refused)` -- the server is unreachable or the port is wrong. On ARM64, try `AWG_NO_GRO=1` -- if the kernel doesn't support GRO, the proxy may hang waiting for a response.

**No traffic after handshake** -- check the NAT rule (`/ip/firewall/nat/print`), routing, and the peer's `endpoint-address` (should be `172.18.0.2`).

**Container keeps restarting** -- set `AWG_LOG_LEVEL=info` and check the logs. Common cause: missing environment variables.

### Storage device not found

If you get `Storage device usb1 not found or has 0 free space` error -- the disk is not formatted or the mount point name doesn't match.

1. Check available disks:

```routeros
/disk/print
```

2. If the disk is visible as a block device but has no partition -- format it as ext4:

```routeros
/disk/format-drive usb1 file-system=ext4 label=usb1
```

3. After formatting, the disk will be available as a mount-point (usually `usb1`). Check the name via `/disk/print` and use it in the configurator ("Container storage" field).

> **Important:** Containers require ext4 filesystem. FAT32 is not supported.

### Insufficient disk space

If you get `Insufficient disk space` error during container installation and you have free space on an external drive (USB, SD, NVMe) -- reconfigure the image download directory:

```routeros
/container/config set tmpdir=usb1/pull ram-high=200M
```

Replace `usb1` with your drive's mount-point (see `/disk/print`).

After the container is installed, you can revert:

```routeros
/container/config set tmpdir="" ram-high=0
```

If using the configurator -- select the appropriate drive in the "Container storage" field, and tmpdir will be configured automatically.

### not allowed by device-mode

The `not allowed by device-mode` error occurs in two cases:

- When creating a container -- container support is not enabled (`container=no`)
- When downloading an image via `/tool/fetch` -- fetch is not enabled (`fetch=no`)

Check the current state:

```routeros
/system/device-mode/print
```

Then enable the required features:

```routeros
/system/device-mode/update container=yes fetch=yes
```

The router will ask for confirmation -- press the Reset or Mode button on the device (depends on model) within a few minutes, or wait for automatic reboot. After reboot, retry the installation.

### child spawn failed / could not load next layer

On devices with 16 MB flash (hAP ac2, hEX, etc.) the container may fail to start with errors:
- `child spawn failed: container run error` or `exited with status 255` (RouterOS 7.20)
- `download/extract error: could not load next layer` (RouterOS 7.21+)

Checklist:

1. **Image format** -- make sure you are using the correct format:
   - RouterOS 7.21+: `awg-proxy-{arch}.tar.gz` (OCI)
   - RouterOS 7.20 and below: `awg-proxy-{arch}-7.20-Docker.tar.gz` (Docker)

2. **tmpdir on USB** -- without this, RouterOS extracts the image to internal flash, which is too small (replace `usb1` with your mount-point from `/disk/print`):
   ```routeros
   /container/config set tmpdir=usb1/pull
   ```

3. **root-dir** -- point to a folder on USB, but **do not create it manually** (RouterOS will create it automatically):
   ```routeros
   /container add ... root-dir=usb1/awg-proxy
   ```

4. **USB format** -- format the drive as ext4:
   ```routeros
   /disk/format-drive usb1 file-system=ext4 label=usb1
   ```

5. **Load from file** -- on devices with 16 MB flash, load the image from a file instead of remote-image:
   ```routeros
   /container add file=awg-proxy-arm.tar.gz ...
   ```

## Building from Source

Requires a C compiler (gcc/musl-gcc), Docker (for container images), and make.

```bash
# Tests
make test

# Local binary build
make build

# Docker images (OCI, for RouterOS 7.21+)
make docker-arm64    # ARM64
make docker-arm      # ARM v7
make docker-armv5    # ARM v5
make docker-amd64    # x86_64
make docker-all      # All architectures

# Docker images (classic format, for RouterOS 7.20 and below)
make docker-arm64-7.20-docker
make docker-arm-7.20-docker
make docker-armv5-7.20-docker
make docker-amd64-7.20-docker
make docker-all-7.20-docker
```

Artifacts are created in the `builds/` directory.

## License

MIT -- see [LICENSE](LICENSE).
