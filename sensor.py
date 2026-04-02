import socket, time, random

def simulate():
    sensors = ["TEMP_01", "VIBR_02", "ENER_03"]
    while True:
        for s_id in sensors:
            try:
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                    s.connect(('api.proyecto-iot.org', 8080))
                    val = round(random.uniform(10, 50), 2)
                    msg = f"DATA|{s_id}|{val}"
                    print(msg)
                    s.sendall(msg.encode())
            except: pass
        time.sleep(3)

if __name__ == "__main__":
    simulate()