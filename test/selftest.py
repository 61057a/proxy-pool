"""proxypool 自测脚本（不需要真实外网，也不需要真的免费代理）

流程：
  1. 起一个假的"代理列表服务器" (127.0.0.1:8001)，返回 127.0.0.1:9090
  2. 起一个假的 HTTP 代理 (127.0.0.1:9090)，GET 返回固定文本，CONNECT 后回 PONG
  3. 用 --no-validate 启动 proxypool，本地端口 8099
  4. 通过 8099 做一次明文 GET 和一次 CONNECT 隧道，检查结果

用法: python test/selftest.py
"""

import socket
import subprocess
import sys
import threading
import time
import urllib.request

ROOT = __file__.rsplit("\\", 2)[0]
EXE = ROOT + r"\dist\proxypool.exe"

PROXY_PORT = 9090
LIST_PORT = 8001
LOCAL_PORT = 8099

stop = threading.Event()


def serve_list():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", LIST_PORT))
    srv.listen(8)
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            c, _ = srv.accept()
        except socket.timeout:
            continue
        try:
            c.recv(8192)
            body = b"127.0.0.1:%d\n" % PROXY_PORT
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                      b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
        finally:
            c.close()
    srv.close()


def serve_fake_proxy():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PROXY_PORT))
    srv.listen(8)
    srv.settimeout(0.5)
    while not stop.is_set():
        try:
            c, _ = srv.accept()
        except socket.timeout:
            continue
        threading.Thread(target=handle_fake, args=(c,), daemon=True).start()
    srv.close()


def handle_fake(c):
    try:
        c.settimeout(10)
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = c.recv(4096)
            if not chunk:
                return
            data += chunk

        if data.startswith(b"CONNECT "):
            c.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            ping = c.recv(1024)
            if ping:
                c.sendall(b"PONG:" + ping)
        else:
            body = b"HELLO-FROM-FAKE-PROXY"
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                      b"Content-Length: %d\r\nConnection: close\r\n\r\n" % len(body) + body)
    except Exception:
        pass
    finally:
        try:
            c.close()
        except Exception:
            pass


def wait_port(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=0.5)
            s.close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def main():
    threading.Thread(target=serve_list, daemon=True).start()
    threading.Thread(target=serve_fake_proxy, daemon=True).start()
    time.sleep(0.5)

    with open(ROOT + r"\build\test_sources.txt", "w") as f:
        f.write("http://127.0.0.1:%d/list.txt\n" % LIST_PORT)

    proc = subprocess.Popen(
        [EXE, "--sources", ROOT + r"\build\test_sources.txt", "--no-validate",
         "--port", str(LOCAL_PORT), "--threads", "4", "--timeout", "2000", "--refresh", "3600"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    failures = []

    if not wait_port(LOCAL_PORT, 25):
        print("FAIL: 本地代理端口没起来")
        proc.kill()
        return 1

    # 1) 明文 HTTP：绝对 URI 转发
    try:
        handler = urllib.request.ProxyHandler({"http": "http://127.0.0.1:%d" % LOCAL_PORT})
        opener = urllib.request.build_opener(handler)
        body = opener.open("http://example.invalid/abc", timeout=10).read()
        if b"HELLO-FROM-FAKE-PROXY" in body:
            print("PASS 明文 GET 转发")
        else:
            failures.append("明文 GET 返回异常: %r" % body)
    except Exception as e:
        failures.append("明文 GET 失败: %s" % e)

    # 2) CONNECT 隧道
    try:
        s = socket.create_connection(("127.0.0.1", LOCAL_PORT), timeout=10)
        s.sendall(b"CONNECT example.com:443 HTTP/1.1\r\nHost: example.com:443\r\n\r\n")
        resp = s.recv(1024)
        if b" 200 " not in resp:
            failures.append("CONNECT 未返回 200: %r" % resp)
        else:
            s.sendall(b"PING\n")
            echoed = s.recv(1024)
            if b"PONG:PING" in echoed:
                print("PASS CONNECT 隧道")
            else:
                failures.append("CONNECT 隧道数据异常: %r" % echoed)
        s.close()
    except Exception as e:
        failures.append("CONNECT 失败: %s" % e)

    # 3) 池空了应返回 5xx 而不是挂住
    stop.set()
    time.sleep(1)
    try:
        s = socket.create_connection(("127.0.0.1", LOCAL_PORT), timeout=10)
        s.sendall(b"GET http://example.invalid/x HTTP/1.1\r\nHost: example.invalid\r\n\r\n")
        resp = s.recv(1024)
        if b" 5" in resp.split(b"\r\n")[0]:
            print("PASS 上游全挂时返回 5xx")
        else:
            failures.append("上游全挂时响应异常: %r" % resp)
        s.close()
    except Exception as e:
        failures.append("上游全挂测试失败: %s" % e)

    proc.kill()
    try:
        out = proc.stdout.read().decode("utf-8", "replace")
    except Exception:
        out = ""
    print("\n--- proxypool 日志 ---")
    print(out[-2000:])

    if failures:
        print("\n结果: FAIL")
        for f in failures:
            print("  - " + f)
        return 1
    print("\n结果: 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
