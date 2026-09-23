# Running a dedicated XRadio server

A dedicated server is a copy of XRadio's relay running on a machine that is not anybody's X-Plane. Pilots connect to it instead of to each other.

Everything here is optional. Read the next section before doing any of it.

---

## Do you actually need one?

Probably not. Hosting from inside the sim does the same job, needs no server, no terminal and no monthly bill: one pilot ticks *Host a flight here* and the others type their address.

A dedicated server earns its keep when:

- **Nobody should have to be "the host".** With in-sim hosting, when the host quits, the flight ends. On a server, people come and go and the flight carries on.
- **The host's router will not open a port.** Though a VPN solves that too, and is free — see [flying together behind a router](vpn.md) first.
- **You want it reachable at any hour**, for a group in different time zones.
- **You want one address that never changes**, instead of re-reading your home address off the Hosting tab every week.

If none of those is true, close this page and tick *Host a flight here*.

---

## What it costs to run

Measured with eight pilots connected:

| | Python server | Built-in (C++) server |
| --- | --- | --- |
| Memory | 21 MB | 16 MB |
| CPU | about 1% of one core | less |

Bandwidth is the only figure worth planning around, and it is small. The server sends each pilot everyone else's position ten times a second:

| Pilots | Out of the server |
| --- | --- |
| 4 | ~0.15 Mbit/s |
| 8 | ~0.5 Mbit/s |
| 20 | ~3.3 Mbit/s |

Voice adds about 0.25 Mbit/s per person talking, to a flight of eight. An evening of eight pilots is a few hundred megabytes in total — nothing against the terabyte most hosting plans include.

**So: the very cheapest VPS is enough.** One shared core and 512 MB of RAM will carry more pilots than you will ever have at once. Do not pay for more.

---

## Where to put it

**A VPS** is the usual answer — €3–5 a month from Hetzner, OVH, DigitalOcean, Vultr, Contabo or any of a hundred others.

Pick the location by **who is flying, not where you live**. Every packet goes to the server and back out again, so the server sits in the middle of the group's latency, not at one end of it. A group spread across Europe wants Frankfurt or Amsterdam; a group split between Europe and the US east coast wants somewhere in between rather than in either home city. 30–60 ms to the server is unnoticeable; 200 ms starts to be heard in the voice.

**A machine at home** — an old PC, a Raspberry Pi, a NAS that runs containers — works and costs nothing. It has the same port problem in-sim hosting has, so either forward UDP 49100 on the router or put everyone on a VPN. See [flying together behind a router](vpn.md).

---

## The five-minute version

### Linux (Debian / Ubuntu)
```bash
apt update && apt install -y python3 curl unzip
mkdir -p /opt/xradio/server
# copy server.py and protocol.py into /opt/xradio/server (see below)
python3 /opt/xradio/server/server.py --host 0.0.0.0 --port 49100 -v
```

### Linux (Arch Linux)
```bash
pacman -Sy --noconfirm python curl unzip
mkdir -p /opt/xradio/server
# copy server.py and protocol.py into /opt/xradio/server (see below)
python3 /opt/xradio/server/server.py --host 0.0.0.0 --port 49100 -v
```

### Linux (Fedora / RHEL / CentOS)
```bash
dnf install -y python3 curl unzip
mkdir -p /opt/xradio/server
# copy server.py and protocol.py into /opt/xradio/server (see below)
python3 /opt/xradio/server/server.py --host 0.0.0.0 --port 49100 -v
```

### Windows (PowerShell)
```powershell
# Using precompiled binary (no Python installation required)
New-Item -ItemType Directory -Force -Path "C:\xradio\server"
# Place xradio_server.exe inside C:\xradio\server\ (see Step 1 below)
Set-Location "C:\xradio\server"
.\win_x64\xradio_server.exe 49100
```

It prints `listening on ('0.0.0.0', 49100)` (or `listening on 0.0.0.0:49100`) and that is a working server. Everything after this point is about keeping it running, keeping strangers out, and not having to think about it again.

---

## Step by step

### 1. Get the files onto the machine

There are only two main Python files (`server.py` and `protocol.py`), and they have no dependencies beyond Python 3.10 or newer. Alternatively, compiled standalone binaries (`xradio_server`) are available for Linux and Windows inside the release zip.

#### On Linux (Bash):

**From the release zip** (contains both Python scripts and compiled binaries):

```bash
mkdir -p /opt/xradio
cd /opt/xradio
curl -LO https://github.com/Anviktor2411/xradio/releases/latest/download/XRadio.zip
unzip XRadio.zip
# now /opt/xradio/XRadio/server/ exists
mv XRadio/server /opt/xradio/server
```

**Or straight from the repository** (Python source only, lightweight):

```bash
mkdir -p /opt/xradio/server && cd /opt/xradio/server
curl -LO https://raw.githubusercontent.com/Anviktor2411/xradio/main/server/server.py
curl -LO https://raw.githubusercontent.com/Anviktor2411/xradio/main/server/protocol.py
```

#### On Windows (PowerShell):

```powershell
New-Item -ItemType Directory -Force -Path "C:\xradio"
Set-Location "C:\xradio"
Invoke-WebRequest -Uri "https://github.com/Anviktor2411/xradio/releases/latest/download/XRadio.zip" -OutFile "XRadio.zip"
Expand-Archive -Path "XRadio.zip" -DestinationPath "C:\xradio"
Move-Item -Path "C:\xradio\XRadio\server" -Destination "C:\xradio\server" -Force
```

> Take both files from the **same version**. `protocol.py` is the wire format; mixing it with a `server.py` from another release is the one way to get a server that starts cleanly and then behaves strangely.

---

### 2. Run it once, by hand

#### Running via Python (Cross-platform):

```bash
python3 /opt/xradio/server/server.py --host 0.0.0.0 --port 49100 -v
```

| | |
| --- | --- |
| `--host 0.0.0.0` | listen on every address the machine has |
| `--port 49100` | the UDP port. Any port works; this is the default pilots expect |
| `--password word` | pilots must give this to join. See [below](#5-set-a-password) |
| `-v` | log every packet type. Useful now, noisy later |

All three can come from the environment instead — `XRADIO_HOST`, `XRADIO_PORT`, `XRADIO_PASSWORD` — which is how systemd and background services parse configurations.

#### Running Precompiled Standalone Binaries (No Python Required):

The release zip carries compiled versions for 64-bit Linux and Windows. They take positional arguments (`port`, then `password`):

* **Linux (x86_64):**
  ```bash
  /opt/xradio/server/lin_x64/xradio_server 49100             # port
  /opt/xradio/server/lin_x64/xradio_server 49100 yourword    # port, then password
  ```

* **Windows (x64):**
  ```powershell
  C:\xradio\server\win_x64\xradio_server.exe 49100            # port
  C:\xradio\server\win_x64\xradio_server.exe 49100 yourword   # port, then password
  ```

The Python and compiled versions are identical in functionality and held to the same test suite, so pick whichever is less trouble to run.

---

### 3. Open the port — in both firewalls

This is where most of the wasted evenings happen. A cloud VPS or home machine usually has **two** firewalls (provider web console + local OS firewall), and the port has to be open in each.

#### A. Provider Firewall (Hetzner, AWS, DigitalOcean, Azure, etc.)
In their web console — look for "Firewall", "Security group", or "Network". Add:
* Protocol: **UDP**
* Port: **49100**
* Source: **0.0.0.0/0** (Anywhere)

#### B. Machine / OS Firewall

* **Debian / Ubuntu / Zorin OS (`ufw`):**
  ```bash
  ufw allow 49100/udp
  ```

* **Fedora / RHEL / CentOS (`firewalld`):**
  ```bash
  firewall-cmd --permanent --add-port=49100/udp
  firewall-cmd --reload
  ```

* **Arch Linux / Generic Linux (`nftables` or `iptables`):**
  ```bash
  # If using ufw:
  ufw allow 49100/udp

  # If using nftables directly:
  nft add rule inet filter input udp dport 49100 accept

  # If using iptables directly:
  iptables -A INPUT -p udp --dport 49100 -j ACCEPT
  ```

* **Windows Firewall (PowerShell as Administrator):**
  ```powershell
  New-NetFirewallRule -DisplayName "XRadio Server UDP" -Direction Inbound -Action Allow -Protocol UDP -LocalPort 49100
  ```

> **UDP, not TCP.** A TCP rule for 49100 does absolutely nothing for XRadio and is the single most common reason a correctly-running server is unreachable.

---

### 4. Check it works before you invite anybody

From your own computer, not the server — this tests the whole path, including both firewalls:

```bash
python3 tools/fake_client.py --server 203.0.113.7 --port 49100 --callsign TEST01
```

`tools/fake_client.py` is in the repository. It needs `protocol.py` beside it, which the checkout has. A working server answers with:

```
logged in as TEST01, sid=1
```

and the server's own log shows:

```
INFO    login: TEST01 (C172) sid=1 from ('198.51.100.4', 51544)
```

If instead you get `no LOGIN_ACK`, nothing has reached the server or nothing has come back. Work through [troubleshooting](#when-it-does-not-work).

Run two copies with different callsigns and positions and each will print the other in its traffic — proof the relay is doing its job, with no sim involved.

---

### 5. Set a password

A server on the open internet with no password is joinable by anyone who finds the port, and they will appear in your flight as an aircraft and a voice.

```bash
python3 server.py --host 0.0.0.0 --port 49100 --password cumulus
```

Pilots put it in **Settings → Connection → Flight password**. A wrong or missing one is refused at login, and the pilot's window says why rather than leaving them guessing.

It is a door lock, not cryptography — the protocol is not encrypted, and anyone who can watch the traffic between a pilot and the server can read the password out of it. It keeps strangers out. Do not reuse a password you use for anything else.

---

### 6. Make it permanent

Ensure the server survives a reboot and restarts automatically if it ever crashes.

#### Option A: Linux (Systemd) — Debian, Ubuntu, Arch Linux, RHEL, Fedora

1. Create a dedicated unprivileged user:
   * **Debian / Ubuntu:**
     ```bash
     adduser --system --no-create-home --group xradio
     ```
   * **Arch Linux / Fedora / RHEL / CentOS:**
     ```bash
     useradd -r -s /usr/bin/nologin xradio
     ```

2. Copy the unit file from the release zip (`server/xradio.service`):
   ```bash
   cp /opt/xradio/server/xradio.service /etc/systemd/system/
   ```

3. Put environment settings in a protected file:
   ```bash
   cat > /etc/xradio.conf <<'EOF'
   XRADIO_PORT=49100
   XRADIO_PASSWORD=cumulus
   EOF
   chmod 600 /etc/xradio.conf
   ```

4. Enable and start:
   ```bash
   systemctl daemon-reload
   systemctl enable --now xradio
   systemctl status xradio          # should say "active (running)"
   ```

5. Watch logs live:
   ```bash
   journalctl -u xradio -f
   ```

---

#### Option B: Windows Service (NSSM or Task Scheduler)

##### Method 1: Using NSSM (Recommended for true service behavior)
1. Download [NSSM (Non-Sucking Service Manager)](https://nssm.cc/) or install via winget:
   ```powershell
   winget install NSSM.NSSM
   ```
2. Install the service pointing to either Python or the precompiled executable:
   ```powershell
   # Using precompiled binary
   nssm install XRadio "C:\xradio\server\win_x64\xradio_server.exe" "49100 cumulus"
   nssm set XRadio AppDirectory "C:\xradio\server"
   nssm start XRadio
   ```

##### Method 2: Using Windows Task Scheduler (No third-party tools)
Run in PowerShell as Administrator to start on system boot without requiring user logon:

```powershell
$action = New-ScheduledTaskAction -Execute "C:\xradio\server\win_x64\xradio_server.exe" -Argument "49100 cumulus"
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "NT AUTHORITY\SYSTEM" -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName "XRadioServer" -Action $action -Trigger $trigger -Principal $principal
Start-ScheduledTask -TaskName "XRadioServer"
```

---

### 7. Tell people how to join

They need two things, and nothing else:

- **Server host** — the machine's address or a domain name pointing at it
- **Port** — 49100, or whatever you chose

Both go in **Settings → Connection**. They do **not** tick *Host a flight here* — that starts a second server on their own machine and connects them to that instead, which is the classic "I put the address in and I am still alone" mistake.

A domain name is worth the five minutes: `fly.example.com` instead of `203.0.113.7`, and if you ever move the server nobody has to be told.

---

## Weather and time

A dedicated server has no sim, so it has no weather of its own. XRadio handles this: the **first pilot to connect with *Offer my weather and time to the flight* switched on** becomes the flight's sky, and everyone with *Fly the flight's weather and time* on follows them.

- The server log says who it picked: `weather and time now come from ESNA12`.
- Each pilot's main window says whose sky they are flying.
- If that pilot leaves, the claim is released and the next pilot offering it picks it up within a few seconds.

If everyone switches *Offer my weather* off, nobody is the sky and each pilot flies their own — which is a valid way to run a server, just be aware it is what you have chosen.

---

## Keeping it up to date

**This is the part that bites.** XRadio's wire protocol changes between releases, and the server speaks exactly one version of it. When pilots update and the server does not, every one of them is refused at login — their window says the versions do not match, and your log fills with:

```
WARNING rejecting ('81.90.144.12', 50413): protocol v4
```

So: **update the server whenever you tell people to update the plugin**, and do it first.

### On Linux:
```bash
systemctl stop xradio
cd /opt/xradio/server
curl -LO https://raw.githubusercontent.com/Anviktor2411/xradio/main/server/server.py
curl -LO https://raw.githubusercontent.com/Anviktor2411/xradio/main/server/protocol.py
systemctl start xradio
journalctl -u xradio -n 20
```

### On Windows (PowerShell):
```powershell
Stop-Service -Name "XRadio"  # or Stop-ScheduledTask -TaskName "XRadioServer"
Set-Location "C:\xradio\server"
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/Anviktor2411/xradio/main/server/server.py" -OutFile "server.py"
Invoke-WebRequest -Uri "https://raw.githubusercontent.com/Anviktor2411/xradio/main/server/protocol.py" -OutFile "protocol.py"
Start-Service -Name "XRadio" # or Start-ScheduledTask -TaskName "XRadioServer"
```

Both files, from the same release, every time. The server keeps no state between runs — no database, no saved sessions — so a restart costs nothing but the few seconds pilots take to log back in automatically.

---

## When it does not work

Work down the list. Each step rules out everything below it.

**1. Is the server actually running?**

* **Linux:**
  ```bash
  systemctl status xradio
  journalctl -u xradio -n 50
  ```
* **Windows:**
  ```powershell
  Get-Service -Name "XRadio"
  # or check Task Manager / Scheduled Tasks
  ```

`listening on ('0.0.0.0', 49100)` means it started. `Address already in use` means something else has the port, or an older copy is still running.

**2. Is it listening where you think?**

* **Linux:**
  ```bash
  ss -lunp | grep 49100
  ```
* **Windows:**
  ```powershell
  Get-NetUDPEndpoint -LocalPort 49100
  ```

Should show `0.0.0.0:49100`. If it shows `127.0.0.1:49100`, `--host` was left at default somewhere and the server is only reachable from itself.

**3. Does anything arrive?** Run the server with `-v` and watch output logs while somebody tries to connect. If **nothing at all appears**, the packets are not reaching the machine and it is a firewall — check both provider and local firewall rules, and verify the rule specifies **UDP**.

**4. Do the replies get back?** If you see `login:` lines in the log but the pilot still says "no answer", the packets are arriving and the replies are not. Almost always an outbound rule on the provider's firewall, or a NAT the server sits behind.

**5. Is everybody on the same version?** A pilot refused at login sees the reason in their own window, and your log names the protocol version they tried. Update the server.

**6. Is the address right?** From the pilot's machine:

```bash
python3 tools/fake_client.py --server <address> --port 49100 --callsign TEST01 --password yourword
```

`logged in as TEST01` means the server is fine and the problem is in the pilot's XRadio settings — most often *Host a flight here* still ticked, which quietly points them at their own machine.

---

## A note on what this server is

It is a relay, and deliberately nothing more. It does not store anything, has no accounts, no database and no web interface; it keeps a list of who is connected in memory and forgets everything when it stops. It is a few hundred lines of Python you can read in one sitting: `server/server.py`.

Two implementations of it exist — the Python one and the one compiled into the plugin — and they speak one protocol. That is exactly the situation where two programs drift apart, so `tools/test_server_parity.py` drives the same scenarios at both over real UDP and fails if the transcripts differ. Whichever you run, the behaviour is the same.

It also accepts input from anyone who can reach the port, so it is written to assume that input is hostile: every field is checked before it is relayed, and CI fires tens of thousands of malformed packets at it under AddressSanitizer on every commit. That is not a promise it is perfect. It is why a password is still a good idea.
