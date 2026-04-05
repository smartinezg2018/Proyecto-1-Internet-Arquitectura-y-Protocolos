"""
operator_dashboard.py — Panel de control para operadores IoT
GUI con Tkinter que muestra sensores, mediciones y alertas en tiempo real.

Uso:
    python operator_dashboard.py                        # conecta a DNS por defecto
    python operator_dashboard.py --host localhost       # prueba local
"""

import tkinter as tk
from tkinter import ttk, scrolledtext
import socket
import threading
import time
import argparse
import sys

DEFAULT_HOST = "eafit-internet-proyecto1.work.gd"
DEFAULT_PORT = 8080


def resolver_host(host: str, port: int):
    """Resuelve el nombre sin IPs hardcodeadas."""
    try:
        info = socket.getaddrinfo(host, port, socket.AF_INET, socket.SOCK_STREAM)
        return info[0][4]
    except socket.gaierror as e:
        return None


class Dashboard:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self.running = True

        # ── Ventana principal ──────────────────────────────────────
        self.root = tk.Tk()
        self.root.title("EAFIT IoT Monitor")
        self.root.geometry("780x520")
        self.root.configure(bg="#1e1e2e")

        # ── Título ─────────────────────────────────────────────────
        tk.Label(
            self.root, text="Sistema de Monitoreo IoT — EAFIT",
            bg="#1e1e2e", fg="#cdd6f4",
            font=("Helvetica", 14, "bold")
        ).pack(pady=(12, 4))

        self.status_var = tk.StringVar(value="Conectando...")
        tk.Label(
            self.root, textvariable=self.status_var,
            bg="#1e1e2e", fg="#a6e3a1", font=("Helvetica", 9)
        ).pack()

        # ── Tabla de sensores ──────────────────────────────────────
        frame_tabla = tk.Frame(self.root, bg="#1e1e2e")
        frame_tabla.pack(fill="both", expand=True, padx=12, pady=8)

        cols = ("ID", "Tipo", "Valor", "Última lectura")
        self.tree = ttk.Treeview(frame_tabla, columns=cols, show="headings", height=8)
        for col in cols:
            self.tree.heading(col, text=col)
            self.tree.column(col, width=170, anchor="center")

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("Treeview",
            background="#313244", foreground="#cdd6f4",
            fieldbackground="#313244", rowheight=26, font=("Helvetica", 10))
        style.configure("Treeview.Heading",
            background="#45475a", foreground="#cdd6f4", font=("Helvetica", 10, "bold"))

        scrollbar = ttk.Scrollbar(frame_tabla, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=scrollbar.set)
        self.tree.pack(side="left", fill="both", expand=True)
        scrollbar.pack(side="right", fill="y")

        # ── Panel de alertas ───────────────────────────────────────
        tk.Label(
            self.root, text="Alertas en tiempo real",
            bg="#1e1e2e", fg="#f38ba8", font=("Helvetica", 10, "bold")
        ).pack(anchor="w", padx=14)

        self.alerts_box = scrolledtext.ScrolledText(
            self.root, height=6, bg="#1e1e2e", fg="#f38ba8",
            font=("Courier", 9), state="disabled", relief="flat",
            insertbackground="#f38ba8"
        )
        self.alerts_box.pack(fill="x", padx=12, pady=(0, 10))

        # ── Arrancar hilos de red ──────────────────────────────────
        threading.Thread(target=self._poll_state,  daemon=True).start()
        threading.Thread(target=self._listen_alerts, daemon=True).start()

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ── Hilo 1: polling de GET_STATE cada 2 segundos ───────────────
    def _poll_state(self):
        while self.running:
            try:
                addr = resolver_host(self.host, self.port)
                if addr is None:
                    self.status_var.set(f"DNS no resuelto para {self.host}")
                    time.sleep(5)
                    continue

                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                    s.settimeout(5)
                    s.connect(addr)
                    s.sendall(b"GET_STATE\n")
                    data = s.recv(4096).decode().strip()

                self.status_var.set(
                    f"Conectado a {addr[0]}:{addr[1]}  —  {time.strftime('%H:%M:%S')}"
                )
                self._actualizar_tabla(data)

            except Exception as e:
                self.status_var.set(f"Sin conexión: {e}")

            time.sleep(2)

    def _actualizar_tabla(self, data: str):
        """Parsea 'id:tipo:valor:hora;...' y actualiza la tabla."""
        filas = []
        for entry in data.split(";"):
            entry = entry.strip()
            if not entry or entry == "EMPTY":
                continue
            partes = entry.split(":", 3)  # maxsplit=3: el timestamp HH:mm:ss tiene ":" internos
            if len(partes) == 4:
                filas.append(partes)
            elif len(partes) == 2:
                # compatibilidad con formato antiguo id:valor
                filas.append([partes[0], "?", partes[1], "--"])

        def update():
            for row in self.tree.get_children():
                self.tree.delete(row)
            for fila in filas:
                self.tree.insert("", "end", values=fila)

        self.root.after(0, update)

    # ── Hilo 2: conexión persistente para alertas push ─────────────
    def _listen_alerts(self):
        """
        Mantiene una conexión abierta con el servidor y espera mensajes
        ALERT|... que el servidor envía proactivamente cuando un sensor
        supera un umbral.
        """
        while self.running:
            try:
                addr = resolver_host(self.host, self.port)
                if addr is None:
                    time.sleep(5)
                    continue

                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                    s.connect(addr)
                    s.sendall(b"SUBSCRIBE\n")
                    ack = s.recv(256).decode().strip()

                    if "OK" not in ack:
                        time.sleep(5)
                        continue

                    # Esperar mensajes push indefinidamente
                    buffer = ""
                    while self.running:
                        chunk = s.recv(1024).decode()
                        if not chunk:
                            break
                        buffer += chunk
                        while "\n" in buffer:
                            linea, buffer = buffer.split("\n", 1)
                            linea = linea.strip()
                            if linea.startswith("ALERT|"):
                                texto = linea[6:]   # quitar "ALERT|"
                                self._mostrar_alerta(texto)

            except Exception:
                pass

            time.sleep(3)

    def _mostrar_alerta(self, texto: str):
        """Agrega una alerta al panel con timestamp."""
        ts = time.strftime("%H:%M:%S")
        msg = f"[{ts}] ⚠  {texto}\n"

        def update():
            self.alerts_box.configure(state="normal")
            self.alerts_box.insert("end", msg)
            self.alerts_box.see("end")
            self.alerts_box.configure(state="disabled")

        self.root.after(0, update)

    # ── Arrancar la app ────────────────────────────────────────────
    def run(self):
        self.root.mainloop()

    def _on_close(self):
        self.running = False
        self.root.destroy()


def main():
    parser = argparse.ArgumentParser(description="Dashboard operador IoT")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    args = parser.parse_args()

    app = Dashboard(args.host, args.port)
    app.run()


if __name__ == "__main__":
    main()