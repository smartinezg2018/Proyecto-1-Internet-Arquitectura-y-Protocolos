"""
iot_clients.py — Clientes de red IoT
Lanza 5 sensores simulados que se conectan al servidor central
y envían mediciones periódicas usando el protocolo diseñado.

Uso:
    python iot_clients.py                         # usa DNS por defecto
    python iot_clients.py --host mi.dominio.org   # host personalizado
    python iot_clients.py --host localhost         # prueba local

Requiere: iot_simulator.py en el mismo directorio.
"""

import socket
import threading
import time
import argparse
import sys

from Sensors.iot_simulator import SensorModel

# ── Configuración ──────────────────────────────────────────────────
DEFAULT_HOST    = "apidominio.proyecto1-iot-eafit.org"
DEFAULT_PORT    = 8080
INTERVALO_SEG   = 3       # segundos entre mediciones
RECONECTAR_SEG  = 5       # espera antes de reintentar conexión


# ── Definición de los 5 sensores del sistema ───────────────────────
SENSORES_CONFIG = [
    ("TEMP_01", "TEMP"),
    ("VIBR_02", "VIBR"),
    ("ENER_03", "ENER"),
    ("HUM_04",  "HUM"),
    ("STAT_05", "STAT"),
]


# ── Resolución de nombre (sin IPs hardcodeadas) ────────────────────
def resolver_host(host: str, port: int):
    """
    Resuelve el nombre de dominio usando getaddrinfo (como exige el proyecto).
    Retorna la dirección resuelta o None si falla.
    """
    try:
        info = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_STREAM)
        return info[0][4]   # (ip, port) resueltos
    except socket.gaierror as e:
        print(f"[DNS] No se pudo resolver '{host}': {e}")
        return None


# ── Hilo de cada sensor ────────────────────────────────────────────
def sensor_thread(sensor_id: str, tipo: str, host: str, port: int):
    """
    Hilo independiente para un sensor:
      1. Resuelve DNS
      2. Abre conexión TCP persistente
      3. Envía REGISTER
      4. Envía DATA cada INTERVALO_SEG segundos
      5. Reconecta automáticamente si pierde la conexión
    """
    modelo = SensorModel(sensor_id, tipo)
    prefix = f"[{sensor_id}]"

    while True:
        addr = resolver_host(host, port)
        if addr is None:
            print(f"{prefix} Reintentando DNS en {RECONECTAR_SEG}s...")
            time.sleep(RECONECTAR_SEG)
            continue

        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(10)
                sock.connect(addr)
                print(f"{prefix} Conectado a {addr[0]}:{addr[1]}")

                # ── 1. Registrar sensor ──────────────────────────
                reg_msg = f"REGISTER|{sensor_id}|{tipo}\n"
                sock.sendall(reg_msg.encode())
                resp = sock.recv(256).decode().strip()
                print(f"{prefix} REGISTER → {resp}")

                if "ERR" in resp:
                    print(f"{prefix} Registro rechazado, cerrando.")
                    break

                # ── 2. Bucle de envío de mediciones ─────────────
                sock.settimeout(None)   # sin timeout para lecturas largas
                while True:
                    valor = modelo.leer()
                    data_msg = f"DATA|{sensor_id}|{tipo}|{valor}\n"
                    # ascii puro: el protocolo es texto plano sin acentos
                    sock.sendall(data_msg.encode("ascii", errors="replace"))

                    ack = sock.recv(256).decode("ascii", errors="replace").strip()
                    alerta = " ⚠ ALERTA" if "ALERT" in ack else ""
                    print(f"{prefix} {tipo}={valor} → {ack}{alerta}")

                    time.sleep(INTERVALO_SEG)

        except (ConnectionRefusedError, ConnectionResetError, BrokenPipeError):
            print(f"{prefix} Conexión perdida. Reconectando en {RECONECTAR_SEG}s...")
        except socket.timeout:
            print(f"{prefix} Timeout. Reconectando en {RECONECTAR_SEG}s...")
        except UnicodeDecodeError as e:
            print(f"{prefix} Error de encoding en respuesta del servidor: {e}")
        except Exception as e:
            print(f"{prefix} Error inesperado: {e}. Reconectando en {RECONECTAR_SEG}s...")

        time.sleep(RECONECTAR_SEG)


# ── Punto de entrada ───────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Clientes sensor IoT")
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"Host del servidor (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT,
                        help=f"Puerto del servidor (default: {DEFAULT_PORT})")
    args = parser.parse_args()

    print(f"=== Iniciando {len(SENSORES_CONFIG)} sensores → {args.host}:{args.port} ===\n")

    hilos = []
    for sensor_id, tipo in SENSORES_CONFIG:
        t = threading.Thread(
            target=sensor_thread,
            args=(sensor_id, tipo, args.host, args.port),
            daemon=True,
            name=f"hilo-{sensor_id}"
        )
        t.start()
        hilos.append(t)
        time.sleep(0.3)     # escalonar conexiones para no saturar el servidor

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[Sistema] Sensores detenidos por el usuario.")
        sys.exit(0)


if __name__ == "__main__":
    main()