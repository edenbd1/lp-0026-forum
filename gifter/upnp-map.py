#!/usr/bin/env python3
"""Keep a one-to-one UPnP TCP port mapping for the forum's gifter, and report
the router's external address.

Forum clients dial the gifter at the address baked into the forum (or set in
its settings), so the gifter's libp2p port has to be reachable from outside:
this maps it on the router and says which external address it got, so the
gifter's status can show the address clients must use.

Standard library only: SSDP discovery, then the WANIPConnection SOAP calls.

  upnp-map.py ensure PORT [LEASE_SECONDS]   map PORT->this host:PORT, print external IP
  upnp-map.py delete PORT                   remove the mapping
  upnp-map.py list                          show the router's mapping table
Exit status 1 when no UPnP gateway answers or the router refuses.
"""

import re
import socket
import sys
import urllib.error
import urllib.request
from urllib.parse import urljoin, urlparse

DESCRIPTION = "logos-forum-gifter"


def discover():
    msg = (
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n"
    ).encode()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(3)
    s.sendto(msg, ("239.255.255.250", 1900))
    try:
        while True:
            data, _ = s.recvfrom(4096)
            m = re.search(rb"(?i)location:\s*(\S+)", data)
            if m:
                return m.group(1).decode()
    except socket.timeout:
        return None


def control(location):
    xml = urllib.request.urlopen(location, timeout=5).read().decode(errors="replace")
    for svc in re.finditer(r"<service>(.*?)</service>", xml, re.S):
        body = svc.group(1)
        st = re.search(r"<serviceType>([^<]+)</serviceType>", body)
        ctl = re.search(r"<controlURL>([^<]+)</controlURL>", body)
        if st and ctl and re.search(r"WAN(IP|PPP)Connection:\d", st.group(1)):
            return st.group(1).strip(), urljoin(location, ctl.group(1).strip())
    return None, None


def soap(st, ctl, action, args):
    body = "".join("<%s>%s</%s>" % (k, v, k) for k, v in args)
    env = (
        '<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
        's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
        '<u:%s xmlns:u="%s">%s</u:%s></s:Body></s:Envelope>' % (action, st, body, action)
    ).encode()
    req = urllib.request.Request(
        ctl, env,
        {"Content-Type": 'text/xml; charset="utf-8"', "SOAPAction": '"%s#%s"' % (st, action)},
    )
    try:
        return True, urllib.request.urlopen(req, timeout=5).read().decode(errors="replace")
    except urllib.error.HTTPError as e:
        return False, e.read().decode(errors="replace")
    except OSError as e:
        return False, str(e)


def local_ip_towards(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((host, 1900))
    return s.getsockname()[0]


def main(argv):
    if len(argv) < 2 or argv[1] not in ("ensure", "delete", "list"):
        print(__doc__.strip(), file=sys.stderr)
        return 2
    loc = discover()
    if not loc:
        print("no UPnP gateway answered", file=sys.stderr)
        return 1
    st, ctl = control(loc)
    if not ctl:
        print("gateway has no WANIPConnection service", file=sys.stderr)
        return 1
    cmd = argv[1]
    if cmd == "list":
        for i in range(256):
            ok, r = soap(st, ctl, "GetGenericPortMappingEntry", [("NewPortMappingIndex", i)])
            if not ok:
                break
            f = dict(re.findall(r"<(New\w+)>([^<]*)</", r))
            print("%s/%s -> %s:%s lease=%s %s" % (
                f.get("NewExternalPort"), f.get("NewProtocol"), f.get("NewInternalClient"),
                f.get("NewInternalPort"), f.get("NewLeaseDuration"), f.get("NewPortMappingDescription")))
        return 0
    port = int(argv[2])
    if cmd == "delete":
        ok, r = soap(st, ctl, "DeletePortMapping",
                     [("NewRemoteHost", ""), ("NewExternalPort", port), ("NewProtocol", "TCP")])
        return 0 if ok else 1
    lease = int(argv[3]) if len(argv) > 3 else 7200
    me = local_ip_towards(urlparse(ctl).hostname)
    args = [("NewRemoteHost", ""), ("NewExternalPort", port), ("NewProtocol", "TCP"),
            ("NewInternalPort", port), ("NewInternalClient", me), ("NewEnabled", 1),
            ("NewPortMappingDescription", DESCRIPTION), ("NewLeaseDuration", lease)]
    ok, r = soap(st, ctl, "AddPortMapping", args)
    if not ok:
        # A stale mapping of ours from a previous run: replace it.
        soap(st, ctl, "DeletePortMapping",
             [("NewRemoteHost", ""), ("NewExternalPort", port), ("NewProtocol", "TCP")])
        ok, r = soap(st, ctl, "AddPortMapping", args)
    if not ok:
        print("router refused the mapping: %s" % re.sub(r"\s+", " ", r)[:300], file=sys.stderr)
        return 1
    ok, r = soap(st, ctl, "GetExternalIPAddress", [])
    ip = re.search(r"<NewExternalIPAddress>([^<]+)<", r) if ok else None
    if not ip:
        print("router did not report an external address", file=sys.stderr)
        return 1
    print(ip.group(1).strip())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
