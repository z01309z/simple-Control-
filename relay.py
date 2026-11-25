import socket, threading

HOST = "0.0.0.0"
PORT = 5555

def pipe(src, dst, name):
    try:
        while True:
            data = src.recv(8192)
            if not data:
                break
            dst.sendall(data)
    except Exception:
        pass
    finally:
        print(f"{name} disconnected.")
        try: src.close()
        except: pass
        try: dst.close()
        except: pass

server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
server.bind((HOST, PORT))
server.listen(5)
print(f"Relay listening on {HOST}:{PORT}")

while True:
    print("Waiting for agent...")
    agent_sock, agent_addr = server.accept()
    print(f"Agent connected from {agent_addr}. Waiting for viewer...")
    
    viewer_sock, viewer_addr = server.accept()
    print(f"Viewer connected from {viewer_addr}. Bridging...")
    
    threading.Thread(target=pipe, args=(agent_sock, viewer_sock, "Agent"), daemon=True).start()
    threading.Thread(target=pipe, args=(viewer_sock, agent_sock, "Viewer"), daemon=True).start()