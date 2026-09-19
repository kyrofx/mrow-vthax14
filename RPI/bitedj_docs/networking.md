# Networking

## SSH access

Configured in `~/.ssh/config` on the workstation, key-based, no password:

```
Host flx4 bitedj pi4
    HostName 10.0.0.190
    User flx4
    IdentityFile ~/.ssh/id_ed25519
    ServerAliveInterval 30
    ServerAliveCountMax 5
```

**The Pi is WiFi-only.** `wlan0` is the sole carrier — there is no Ethernet cable. Every
remote-administration assumption depends on the wireless link, so changing networks
means losing access until you know the new address.

`avahi-daemon` is enabled and `flx4.local` resolves, which is the fallback when the IP
changes. The `ssh` config deliberately pins the IP because `.local` has been flaky on
this LAN; treat mDNS as a recovery path, not the primary one.

## Staging networks for venues you have not visited

The appliance moves between locations, and the network it needs is usually out of range
when you configure it. NetworkManager will happily hold a profile for a network it has
never seen: adding one does not scan, associate, or disturb the current link.

```bash
add-wifi <SSID>                  # prompts for the password, not echoed
add-wifi <SSID> --hidden         # non-broadcasting network
add-wifi <SSID> --priority 10    # wins when several are in range
add-wifi --list
add-wifi --forget <SSID>
```

`~/.local/bin/add-wifi` reads the password from a hidden prompt rather than `argv`, so
it stays out of shell history and out of `ps`. It writes `wifi-sec.psk-flags 0` so the
secret lives in the system connection file and the box can associate **at boot with
nobody logged in** — an agent-owned secret would never be answered on an appliance.

Profiles land in `/etc/NetworkManager/system-connections/<SSID>.nmconnection`, mode
`600`, owned by root.

### Priorities

All profiles currently sit at priority `0`, so NetworkManager connects to whichever
known network is in range. Set `--priority` only if you need a deterministic winner
where two known networks overlap.

### Coexistence with netplan

The originally-provisioned network is a netplan-generated profile
(`netplan-wlan0-<ssid>`), which lives under `/run/NetworkManager/system-connections/`.
Anything `add-wifi` creates goes to `/etc/NetworkManager/system-connections/` instead.
These are separate files and coexist fine — netplan will not clobber hand-added
profiles, and `add-wifi --list` shows both.

## Does the build need the network?

Only for `KEYFINDER`, and only once. CMake fetches and builds `libkeyfinder` plus FFTW
as an ExternalProject. Once this exists:

```
~/bitedj/build/libdjinterop-2.2.8/src/libkeyfinder-stamp/libkeyfinder-done
```

the fetch is complete and the remainder of the build is entirely local. Dropping the
network mid-build is safe after that point.

The build itself survives losing SSH regardless, because it is launched detached with
`setsid` — see [build.md](build.md#compile). You lose visibility, not progress.

## Moving the box to a new network

1. Stage the SSID with `add-wifi` **before** you move it.
2. After the move, find it again by `flx4.local`, or by checking the new network's DHCP
   leases.
3. Update `HostName` in `~/.ssh/config` if the new address is stable and you want the
   shortcut to keep working.
