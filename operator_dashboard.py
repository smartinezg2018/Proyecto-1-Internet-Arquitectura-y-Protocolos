import tkinter as tk
from tkinter import ttk
import socket, threading, time

def update_ui():
    while True:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
                s.connect(('localhost', 8080))
                s.sendall(b"GET_STATE")
                data = s.recv(1024).decode()
                
                # Limpiar y actualizar tabla
                for i in tree.get_children(): tree.delete(i)
                for entry in data.split(';'):
                    if ':' in entry:
                        node, val = entry.split(':')
                        tree.insert('', 'end', values=(node, val, time.strftime('%H:%M:%S')))
        except: pass
        time.sleep(2)

root = tk.Tk()
root.title("EAFIT IoT Dashboard")
tree = ttk.Treeview(root, columns=('ID', 'VAL', 'TIME'), show='headings')
tree.heading('ID', text='Sensor ID'); tree.heading('VAL', text='Valor'); tree.heading('TIME', text='Hora')
tree.pack(padx=10, pady=10)

threading.Thread(target=update_ui, daemon=True).start()
root.mainloop()