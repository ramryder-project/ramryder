#!/usr/bin/env python3
import socket
import subprocess


class ResourceClient:
    def __init__(self, socket_path: str = "/var/run/resource_manager.sock") -> None:
        self.socket_path = socket_path

    def send(self, command: str) -> str:
        print(f"[cmd] {command}")
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(self.socket_path)
            sock.sendall(command.encode("utf-8"))
            data = sock.recv(65536)
            return data.decode("utf-8", errors="replace").strip()
        except PermissionError:
            return self._send_via_sudo_python(command)
        finally:
            sock.close()

    def _send_via_sudo_python(self, command: str) -> str:
        helper = (
            "import socket,sys;"
            "p=sys.argv[1];c=sys.argv[2];"
            "s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM);"
            "s.connect(p);s.sendall(c.encode());"
            "d=s.recv(65536);"
            "sys.stdout.write(d.decode(errors='replace').strip());"
            "s.close()"
        )
        proc = subprocess.run(
            ["sudo", "python3", "-c", helper, self.socket_path, command],
            capture_output=True,
            text=True,
        )
        if proc.returncode != 0:
            err = (proc.stderr or "").strip()
            raise RuntimeError(f"resource client sudo fallback failed: {err or proc.returncode}")
        return (proc.stdout or "").strip()

    def get_vms(self) -> str:
        return self.send("get-vms")

    def get_mem_pool(self) -> str:
        return self.send("get-mem-pool")

    def get_num_nodes(self) -> str:
        return self.send("get-num-nodes")

    def get_node_info(self, nid: int) -> str:
        return self.send(f"get-node-info nid={nid}")

    def create_vm(self, vid: int, coreset: str) -> str:
        return self.send(f"create-vm vid={vid} coreset=[{coreset}]")

    def destroy_vm(self, vid: int) -> str:
        return self.send(f"destroy-vm vid={vid}")

    def start_vm(self, vid: int) -> str:
        return self.send(f"start-vm vid={vid}")

    def alloc_mem(self, nid: int, vid: int, size_mb: int) -> str:
        return self.send(f"alloc-mem nid={nid} vid={vid} size={size_mb}")

    def free_mem(self, vid: int, memid: int) -> str:
        return self.send(f"free-mem vid={vid} memid={memid}")

    def attach_mem(self, memid: int, vid: int, nid: int) -> str:
        return self.send(f"attach-mem memid={memid} vid={vid} nid={nid}")

    def detach_mem(self, memid: int, vid: int) -> str:
        return self.send(f"detach-mem memid={memid} vid={vid}")
