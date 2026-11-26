# Flying XRadio over Tailscale

Tailscale lets you fly together with XRadio when your router won't open a
port. That covers no access to the router, mobile internet, Starlink, carrier
NAT, or a university or dorm network.

Tailscale puts every pilot's computer on one small private network. Each
machine gets an extra address that starts with `100.` (for example
`100.94.3.17`), and those addresses can reach each other wherever the machines
are. XRadio sees everyone as if they were on the same LAN, so nobody has to
touch a router.

**Cost:** free. The Personal plan covers up to 6 users, with as many devices
each as you like.

---

## Quick version

| Who | What |
| --- | --- |
| **Host** | Install Tailscale → invite friends as **users** → host in XRadio with the router option **off** → send your `100.x.x.x:49100` |
| **Everyone else** | Install Tailscale → accept the invite → put the host's `100.x.x.x` and port in XRadio → Apply |

---

## Host

### 1. Install Tailscale

Download it from [tailscale.com/download](https://tailscale.com/download) for
Windows, macOS or Linux, then sign in with Google, Microsoft or GitHub. You
don't need to make a separate account.

### 2. Invite your friends as users

Open [login.tailscale.com](https://login.tailscale.com) and go to
**Users → Invite users**. Send each friend an invite link or email.

> ⚠️ **Don't use the *Share* button on your machine.** A shared machine is
> quarantined: it can answer connections but can't start them. The XRadio
> server has to send packets to every pilot, so sharing breaks it. Invite
> people as **users** instead.

### 3. Find your Tailscale address

Open the Tailscale app (tray icon) or the admin console. The address next to
your machine starts with `100.`, for example `100.94.3.17`.

### 4. Host the flight in XRadio

**New window (Dear ImGui), Host / Connect tab**

- ✅ **Host a flight**, port `49100` (the default)
- ❌ **Ask the router to open the port**: untick it, there's nothing to open
- Optional: set a **Password**
- Press **Apply**

**Classic window: Plugins → XRadio → Settings → Hosting**

- ✅ **Host a flight here**
- ❌ **Ask the router to open it**
- Press **Save & apply**

The window should now say `Running … Port 49100`.

### 5. Send your address to the others

```
100.94.3.17:49100
```

Use **your own** `100.` address. Don't send your router's address
(`192.168.x.x`) or your public IP.

---

## Everyone else

1. **Install Tailscale** and **accept the host's invite**. Sign in with the
   same kind of account the invite was sent to.
2. In X-Plane, open XRadio:
   - **New window → Host / Connect → Connect to:** put the host's Tailscale
     address in *Server address or join code*, set **Port** to `49100`, and
     press **Apply**.
   - **Classic window → Settings → Connection:** same fields, then
     **Save & apply**.
3. The main window should say:

   ```
   connected to 100.94.3.17:49100
   ```

✈️ You're in.

---

## Check it before loading the sim

From a friend's computer, ping the host's Tailscale address:

```bash
ping 100.94.3.17
```

- **Reply** means the tunnel works. If XRadio still won't connect, the
  problem is in its settings or a firewall.
- **No reply** means Tailscale isn't running on one of the two machines, or
  the invite wasn't accepted.

---

## Troubleshooting

Go through these in order, because each step rules out the ones below it.

1. **Ping fails.** Make sure Tailscale is running and logged in on both
   machines, and that both appear in the admin console under the same
   network.
2. **The host isn't actually hosting.** The Host tab should say
   `Running … Port 49100`. If it doesn't, the setting wasn't applied.
3. **Ping works but XRadio won't connect.** Almost always the host's
   **Windows Firewall** is blocking it. Allow X-Plane through for
   **private networks**, or allow **UDP 49100** inbound. Third-party
   antivirus or "internet security" firewalls can block it too.
4. **"Version mismatch" at login.** Everyone needs the same XRadio
   version. Update together.
5. **Voice stutters for only one pilot.** XRadio keeps every packet under
   1200 bytes so it fits through a WireGuard tunnel like Tailscale's. If
   someone has lowered the tunnel MTU below the usual 1280, look there
   first.

Still stuck? Open an issue at
[github.com/Anviktor2411/xradio/issues](https://github.com/Anviktor2411/xradio/issues).
Include what the Host tab says and the XRadio lines from
`X-Plane 12/Log.txt`.

---

## FAQ

**Does everyone need Tailscale?**
Yes. Every pilot who connects, and the host, needs it running and needs to be
on the same tailnet.

**Is it slower than a direct connection?**
Usually not. Tailscale connects machines directly whenever it can. If a
direct path isn't possible, it falls back to its relay servers (DERP), which
adds some latency but still works fine for voice and positions.

**Does the address change?**
No. A machine's `100.x` address stays the same, so friends can save it once.

**Do I need to forward ports or change anything on the router?**
No. That's the whole point.

**What if a friend can't install Tailscale?**
ZeroTier works the same way. See [vpn.md](vpn.md) for that and the other
options (port forwarding, or a VPS that's always up).
