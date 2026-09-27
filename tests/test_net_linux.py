"""#831: the Linux net backend parses iw/wpa_cli/ip output via a fake runner.

net_linux.c is compiled directly (the Makefile only links it on Linux) and run
with MMB_NET_CMD_DIR pointing at fake tools, so this works on any host.
"""

import os
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INCLUDES = [
    "-DMMB_PLATFORM_POSIX",
    "-I",
    os.path.join(REPO, "native"),
    "-I",
    os.path.join(REPO, "mmbasic", "include"),
    "-I",
    os.path.join(REPO, "mmbasic", "src"),
    "-I",
    os.path.join(REPO, "mmbasic", "third_party"),
    "-I",
    os.path.join(REPO, "console"),
]

IP_FAKE = """\
iface="$5"
case "$iface" in
  wlan0)
    echo "2: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500"
    echo "    inet 192.0.2.55/24 brd 192.0.2.255 scope global wlan0"
    ;;
  eth0)
    echo "2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500"
    echo "    inet 198.51.100.7/24 brd 198.51.100.255 scope global eth0"
    ;;
  *) exit 1 ;;
esac
"""

IW_OK = """\
cat <<'EOF'
BSS aa:bb:cc:dd:ee:01(on wlan0)
\tSSID: NetOne
BSS aa:bb:cc:dd:ee:02(on wlan0)
\tSSID: Net Two
BSS aa:bb:cc:dd:ee:01(on wlan0)
\tSSID: NetOne
EOF
"""

IW_FAIL = "exit 1\n"

WPA_OK = """\
cat <<'EOF'
bssid / frequency / signal level / flags / ssid
aa:bb:cc:dd:ee:01\t2437\t-40\t[WPA2-PSK-CCMP][ESS]\tWpaOne
aa:bb:cc:dd:ee:02\t2437\t-50\t[WPA2-PSK-CCMP][ESS]\tWpa Two
EOF
"""


def _write_exe(path, body):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("#!/bin/sh\n" + body)
    os.chmod(path, 0o755)


def _build(tmp_path):
    exe = os.path.join(str(tmp_path), "net_linux_host")
    subprocess.run(
        [
            "cc",
            "-O0",
            "-Wall",
            "-Werror",
            *INCLUDES,
            "-o",
            exe,
            os.path.join(REPO, "tests", "net_linux_host.c"),
            os.path.join(REPO, "native", "net_linux.c"),
        ],
        check=True,
        cwd=REPO,
    )
    return exe


def _fake_dir(tmp_path, name, iw_body):
    d = os.path.join(str(tmp_path), name)
    os.makedirs(d, exist_ok=True)
    _write_exe(os.path.join(d, "iw"), iw_body)
    _write_exe(os.path.join(d, "wpa_cli"), WPA_OK)
    _write_exe(os.path.join(d, "ip"), IP_FAKE)
    _write_exe(os.path.join(d, "rc-service"), "exit 0\n")
    return d


def _env(cmd_dir, wpa_conf=None):
    env = dict(os.environ)
    env["MMB_NET_CMD_DIR"] = cmd_dir
    env["MMB_NET_WLAN_IFACE"] = "wlan0"
    env["MMB_NET_ETH_IFACE"] = "eth0"
    if wpa_conf:
        env["MMB_WPA_CONF"] = wpa_conf
    return env


def test_net_linux_iw_scan_status(tmp_path):
    exe = _build(tmp_path)
    cmd_dir = _fake_dir(tmp_path, "fake", IW_OK)
    out = subprocess.run(
        [exe], check=True, capture_output=True, text=True, env=_env(cmd_dir)
    )
    assert "all checks passed" in out.stdout


def test_net_linux_wpa_cli_fallback(tmp_path):
    exe = _build(tmp_path)
    cmd_dir = _fake_dir(tmp_path, "fake_fallback", IW_FAIL)
    out = subprocess.run(
        [exe, "wpa"],
        check=True,
        capture_output=True,
        text=True,
        env=_env(cmd_dir),
    )
    assert "all checks passed" in out.stdout


def test_net_linux_connect_writes_config_and_restarts(tmp_path):
    exe = _build(tmp_path)
    cmd_dir = _fake_dir(tmp_path, "fake_connect", IW_OK)
    wpa_conf = os.path.join(str(tmp_path), "wpa_supplicant.conf")
    out = subprocess.run(
        [exe, "connect"],
        check=True,
        capture_output=True,
        text=True,
        env=_env(cmd_dir, wpa_conf),
    )
    assert "all checks passed" in out.stdout
