"""Find which process listens on a TCP port, using /proc (Linux only).

The robot program's working directory is the robot project, which is where
the Sim GUI's layout files live. Listening sockets in /proc/net/tcp{,6} carry
an inode; the owning process has a file descriptor that links to
"socket:[<inode>]".
"""

from __future__ import annotations

import os
from pathlib import Path

PROC = Path("/proc")


def _listening_inodes(port: int) -> set[str]:
    inodes: set[str] = set()
    for table in ("net/tcp", "net/tcp6"):
        try:
            lines = (PROC / table).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            cols = line.split()
            # sl local_address rem_address st ... inode
            if len(cols) < 10 or cols[3] != "0A":  # 0A == TCP_LISTEN
                continue
            try:
                local_port = int(cols[1].rsplit(":", 1)[1], 16)
            except (IndexError, ValueError):
                continue
            if local_port == port:
                inodes.add(cols[9])
    return inodes


def port_listening(port: int) -> bool | None:
    """Whether some process listens on `port`, read from the socket tables.

    None when the tables are not available (not Linux), in which case the
    caller has to find out some other way.
    """
    if not (PROC / "net" / "tcp").is_file():
        return None
    return bool(_listening_inodes(port))


def pid_listening_on(port: int) -> int | None:
    """PID of a process (visible to this user) listening on `port`, if any."""
    inodes = _listening_inodes(port)
    if not inodes:
        return None
    targets = {f"socket:[{inode}]" for inode in inodes}
    try:
        pids = [int(name) for name in os.listdir(PROC) if name.isdigit()]
    except OSError:
        return None
    for pid in sorted(pids, reverse=True):
        fd_dir = PROC / str(pid) / "fd"
        try:
            for fd in os.listdir(fd_dir):
                try:
                    if os.readlink(fd_dir / fd) in targets:
                        return pid
                except OSError:
                    continue
        except OSError:
            continue  # not ours, or exited
    return None


def process_cwd(pid: int) -> Path | None:
    try:
        return Path(os.readlink(PROC / str(pid) / "cwd"))
    except OSError:
        return None


def process_start_time(pid: int) -> float | None:
    """Start time as seconds since the epoch, from the /proc entry's ctime."""
    try:
        return (PROC / str(pid)).stat().st_ctime
    except OSError:
        return None
