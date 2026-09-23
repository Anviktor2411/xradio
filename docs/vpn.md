# Flying together when the router says no

XRadio normally sorts hosting out by itself: one pilot ticks *Host a flight
here*, the plugin asks the router to open the port, and the window shows an
address for everyone else to type. This page is for when that does not work —
when the Hosting tab says something like:

```
  Port not opened: no router answered (UPnP and NAT-PMP are both switched off on it)
  Friends type:  192.168.1.20:49100   (on your network)
```

That address works for somebody sitting in the same house. It is no use at all
to a friend in another country, and passing it on is the most common way an
evening gets wasted.

Nothing here is a limitation of XRadio. It is the ordinary problem of running
any server from home, and there are four ways out.

---

## Which one is for me?

Read down. Stop at the first one that fits.

| If… | Do this |
| --- | --- |
| You can get into your router's settings page | **[Forward the port](#4-forward-the-port-yourself)** — ten minutes once, then nothing to think about again |
| You cannot, or it is your landlord's / your parents' / the university's router | **[Put everyone on a VPN](#1-tailscale-recommended)** — Tailscale, about five minutes per person |
| You are on mobile internet, Starlink, or the window said *carrier NAT* | **[Put everyone on a VPN](#1-tailscale-recommended)** — forwarding cannot work from there, whatever the router says |
| Somebody else in the group has a router they *can* forward on | **Let them host.** Simplest answer on this page |
| You want a server that is up whether or not any particular person is online | **[Rent a small VPS](#5-a-vps-that-is-always-up)** — about €4 a month |

A VPN sounds like the heavy option and is in fact the easy one: nothing is
configured on anybody's router, and it works from behind carrier NAT, which
port forwarding cannot.

---

## 1. Tailscale (recommended)

Tailscale puts every machine in the group on one small private network. Each
computer gets an extra address of its own — something like `100.94.3.17` — and
those addresses reach each other directly, wherever the machines actually are.
As far as XRadio is concerned, everyone is suddenly on the same LAN.

It is free for this: up to six people, as many computers each as you like, on
the Personal plan. It is explicitly meant for, among other things, playing
games with friends.

### What the host does

1. **Install it.** Go to [tailscale.com/download](https://tailscale.com/download),
   get the installer for Windows, macOS or Linux, and run it. Sign in with a
   Google, Microsoft or GitHub account — there is no separate password to
   invent.
2. **Invite the others.** Open [login.tailscale.com](https://login.tailscale.com),
   go to **Users → Invite users**, and send each friend an invite by email or
   link. They sign in the same way and their machines join the same network.

   > Tailscale also has a *Share* button on an individual machine. Do not use
   > that one here. A shared machine is quarantined by default — it can accept
   > connections but not start them — and XRadio's server has to send to every
   > pilot, not only answer them. Inviting people as **users** puts everyone on
   > one network with no such restriction.

3. **Find your Tailscale address.** Open the Tailscale app (or the admin
   console) and look at the address next to your own machine. It always starts
   with `100.` — for example `100.94.3.17`.
4. **Host as normal, but turn the router question off.** In X-Plane:
   **Plugins → XRadio → Settings → Hosting**
   - tick **Host a flight here**
   - untick **Ask the router to open it** — there is nothing to open, and
     leaving it on only means waiting a few seconds for the router to say no
   - **Save & apply**
5. **Send the others your Tailscale address and port**, e.g.
   `100.94.3.17:49100`.

### What everyone else does

1. Install Tailscale and accept the invitation.
2. In X-Plane: **Plugins → XRadio → Settings → Connection**, put the host's
   Tailscale address in **Server host or join code**, set **Port** to the
   host's port, and **Save & apply**.

That is all. The main window should say `connected to 100.94.3.17:49100`.

### Checking it before you load the sim

Worth doing once, because it separates "the VPN is not up" from "XRadio is
misconfigured":

```bash
ping 100.94.3.17            # the host's Tailscale address, from a friend's machine
```

A reply means the tunnel is up and anything still wrong is in XRadio's
settings. No reply means Tailscale is not running on one of the two machines,
or the invitation was never accepted.

### Windows firewall

Tailscale's installer normally handles this, but if the tunnel pings and
XRadio still cannot connect, the host's firewall is the next suspect. Allow
X-Plane through it for **private networks**, or open UDP 49100 inbound. The
same goes for any third-party firewall or "internet security" suite.

---

## 2. ZeroTier

Same idea, different company, and a reasonable fallback if Tailscale will not
install on someone's machine. The free plan covers **10 devices on one
network** — enough for a group of ten pilots, or five with a laptop each.

1. One person makes an account at
   [my.zerotier.com](https://my.zerotier.com), creates a network, and copies
   its **16-character network ID**.
2. Everyone installs ZeroTier from
   [zerotier.com/download](https://www.zerotier.com/download/) and joins that
   network ID.
3. The person who made the network goes back to the web page and **ticks each
   member** under *Members* to let them in. This step is easy to miss and is
   why "I joined and nothing happened" usually happens.
4. Each machine now has an extra address — by default in the `10.147.x.x`
   range. The host uses theirs in XRadio exactly as in the Tailscale steps
   above: host with *Ask the router to open it* unticked, and give the others
   `10.147.17.5:49100` or whatever it shows.

---

## 3. Any other VPN

Anything that gives every machine an address the others can reach will do:
WireGuard set up by hand, Netbird, a self-hosted Headscale, Hamachi, even an
existing corporate VPN if it allows machines to talk to each other. The
procedure never changes:

1. Everyone gets onto the same virtual network.
2. The host finds their address **on that network** — not the one the router
   gave them, and not their public one.
3. Host with *Ask the router to open it* unticked and hand that address out.

One thing to check if you are rolling your own: XRadio keeps every packet
under **1200 bytes** precisely so it fits inside a WireGuard-style tunnel
without being fragmented. If your VPN sets a smaller link than the usual 1280,
traffic may stutter for one pilot and not another. That is the first thing to
look at if the symptoms are strange and one-sided.

---

## 4. Forward the port yourself

If you can get into the router, this is still the best answer: no extra
software on anyone's machine, and friends can join with nothing installed.

XRadio has already worked out and printed both numbers you need. The Hosting
tab says, in so many words:

```
  Over the internet: forward UDP 49100 on your router to 192.168.1.20,
  then friends type  81.90.144.12:49100
```

- `49100` — the port, **UDP** (not TCP; a rule for TCP does nothing here)
- `192.168.1.20` — the machine running X-Plane, on your own network
- `81.90.144.12` — your public address, which is what friends type

In the router's settings page the section is usually called *Port forwarding*,
*Virtual servers*, *NAT forwarding* or *Applications & gaming*. Make one rule:
protocol UDP, external port 49100, internal port 49100, internal address
`192.168.1.20`.

Two things that commonly go wrong afterwards:

- **The Windows firewall still blocks it.** Allow X-Plane through, for private
  *and* public networks.
- **Your public address changes.** Most home connections get a new one every
  so often. Check the Hosting tab before each session rather than reusing last
  week's address — or give friends the **join code**, which is the same
  information in eleven characters, and use the *Copy join code* button so you
  do not have to read it out.

### Why it sometimes cannot work at all

If the Hosting tab says your router is itself behind another NAT, stop here
and use a VPN. On mobile internet, Starlink and a growing number of fibre
providers, your "public" address is shared with hundreds of other customers
and you have no control over the equipment that holds it. No amount of
forwarding on your own router changes that. XRadio detects this and says so,
rather than handing you an address that goes nowhere.

---

## 5. A VPS that is always up

Worth it only if you want a server that does not depend on one particular
person being online. Any €4/month box will do — XRadio's server is a few
hundred lines of Python and idles at nothing.

```bash
cd server
python3 server.py --host 0.0.0.0 --port 49100 -v
```

Python 3.10 or newer, no dependencies. Open **UDP** 49100 in the provider's
firewall as well as the machine's. A `systemd` unit is in
`server/xradio.service` so it comes back after a reboot. The release package
also carries the same server as a compiled binary if you would rather not
install Python:

```bash
./server/lin_x64/xradio_server 49100
```

Then everyone puts the VPS's address in **Settings → Connection**. Nobody
hosts; nobody's router is involved.

A dedicated server has no sim and therefore no weather of its own. XRadio
handles that: the first pilot to connect with **Offer my weather and time to
the flight** switched on becomes the flight's sky, and everyone else follows
it. The main window's *Weather and time* line says who that is.

Set a **flight password** on the server (`--password yourword`) if it is
reachable from the open internet — otherwise anyone who finds the port can
join.

---

## Still stuck?

Work through it in this order, because each step rules out everything below
it:

1. **Is the tunnel up?** `ping` the host's VPN address from a friend's
   machine. No reply → the problem is the VPN, not XRadio.
2. **Is the host actually hosting?** Their Hosting tab should say
   `Running on port 49100`. If not, *Host a flight here* did not save.
3. **Is everyone on the same version?** A mismatched build is refused at login
   and the window says so in as many words. The protocol changes between
   releases; everyone has to update together.
4. **Is the firewall in the way?** This is nearly always the answer when the
   ping works and the connection does not. Allow X-Plane, or UDP 49100
   inbound, on the host's machine.
5. **Ask the router yourself.** `python3 tools/upnp_probe.py` asks your router
   the same questions XRadio does, from outside the sim, and says whether the
   silence is the router's or a bug here.

If none of that does it, open an issue at
[github.com/Anviktor2411/xradio/issues](https://github.com/Anviktor2411/xradio/issues)
with what the Hosting tab says and the relevant lines from
`X-Plane 12/Log.txt`.
