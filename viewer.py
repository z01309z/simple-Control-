import socket, struct, threading, sys
import numpy as np
import cv2

def recvall(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf

def send_pkt(sock, data):
    try:
        sock.sendall(data)
    except Exception:
        pass

class Remote:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.screen_w = 0
        self.screen_h = 0
        self.frame = None
        self.lock = threading.Lock()
        self.running = True
        threading.Thread(target=self._recv_loop, daemon=True).start()

    def _recv_loop(self):
        while self.running:
            hdr = recvall(self.sock, 4)
            if not hdr:
                break
            (ln,) = struct.unpack("<I", hdr)
            payload = recvall(self.sock, ln)
            if not payload:
                break
            arr = np.frombuffer(payload, dtype=np.uint8)
            img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
            if img is None:
                continue
            with self.lock:
                self.frame = img
                self.screen_h, self.screen_w = img.shape[:2]

    def send_mouse(self, x, y):
        if self.screen_w == 0: return
        x = max(0, min(self.screen_w - 1, x))
        y = max(0, min(self.screen_h - 1, y))
        send_pkt(self.sock, struct.pack("<Bhh", 0x01, x, y))

    def send_button(self, btn, down):
        send_pkt(self.sock, struct.pack("<BBB", 0x02, btn, 1 if down else 0))

    def send_wheel(self, delta):
        send_pkt(self.sock, struct.pack("<Bh", 0x03, delta))

    def send_key(self, vk, down):
        send_pkt(self.sock, struct.pack("<BHB", 0x04, vk, 1 if down else 0))

    def close(self):
        self.running = False
        try:
            self.sock.close()
        except Exception:
            pass

def main():
    if len(sys.argv) < 2:
        print("usage: viewer.py <relay-ip> [port]")
        sys.exit(1)
    host = sys.argv[1]
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5555
    r = Remote(host, port)

    win = "remote"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    cv2.resizeWindow(win, 1280, 720)

    def on_mouse(event, x, y, flags, param):
        with r.lock:
            fw, fh = r.screen_w, r.screen_h
        if fw == 0: return
        try:
            _, _, ww, wh = cv2.getWindowImageRect(win)
        except Exception:
            ww, wh = fw, fh
        if ww <= 0 or wh <= 0: return
        ix = int(x * fw / ww)
        iy = int(y * fh / wh)
        
        if event == cv2.EVENT_MOUSEMOVE:
            r.send_mouse(ix, iy)
        elif event == cv2.EVENT_LBUTTONDOWN:
            r.send_mouse(ix, iy); r.send_button(0, True)
        elif event == cv2.EVENT_LBUTTONUP:
            r.send_mouse(ix, iy); r.send_button(0, False)
        elif event == cv2.EVENT_RBUTTONDOWN:
            r.send_mouse(ix, iy); r.send_button(1, True)
        elif event == cv2.EVENT_RBUTTONUP:
            r.send_mouse(ix, iy); r.send_button(1, False)
        elif event == cv2.EVENT_MBUTTONDOWN:
            r.send_mouse(ix, iy); r.send_button(2, True)
        elif event == cv2.EVENT_MBUTTONUP:
            r.send_mouse(ix, iy); r.send_button(2, False)
        elif event == cv2.EVENT_MOUSEWHEEL:
            delta = 120 if flags > 0 else -120
            r.send_wheel(delta)

    cv2.setMouseCallback(win, on_mouse)

    keymap = {
        13: 0x0D, 27: 0x1B, 8: 0x08, 9: 0x09, 32: 0x20, 127: 0x2E,
    }

    while True:
        with r.lock:
            frame = r.frame
        if frame is not None:
            cv2.imshow(win, frame)
        k = cv2.waitKey(1) & 0xFF
        if k == 27: break
        elif k != 255:
            vk = keymap.get(k)
            if vk is None and 32 <= k < 127: vk = k
            if vk:
                r.send_key(vk, True)
                r.send_key(vk, False)
        if cv2.getWindowProperty(win, cv2.WND_PROP_VISIBLE) < 1:
            break

    r.close()
    cv2.destroyAllWindows()

if __name__ == "__main__":
    main()