# Sistema Distribuido de Monitoreo de Sensores IoT

Proyecto 1 — Internet: Arquitectura y Protocolos  
Universidad EAFIT

Sistema de monitoreo IoT distribuido desplegado en AWS, compuesto por un servidor central en C, sensores simulados en Python, panel de control para operadores en Java y Python, servicio de autenticación en Java e interfaz web con login.

---

## Arquitectura del sistema

```
                        DNS
              (eafit-internet-proyecto1.work.gd)
                           │
              ┌────────────▼────────────┐
              │        AWS EC2          │
              │  ┌──────────────────┐   │
              │  │  Docker Compose  │   │
              │  │                  │   │
              │  │  iot-server:8080 │◄──┼── browser / sensores / operadores
              │  │  (server.c en C) │   │
              │  │                  │   │
              │  │ auth-service:9000│   │
              │  │ (AuthServer.java)│   │
              │  └──────────────────┘   │
              └─────────────────────────┘
                      │         │
          ┌───────────┘         └──────────────┐
          │                                    │
  Sensores IoT (Python)           Operadores (Java / Python)
  iot_clients.py                  OperatorDashboard.java
  5 sensores simultáneos          operator_dashboard.py
```

### Componentes

| Archivo | Lenguaje | Rol |
|---|---|---|
| `server.c` | C | Servidor central: sockets, protocolo, HTTP, logs |
| `iot_simulator.py` | Python | Modelo de simulación de sensores (valores realistas) |
| `iot_clients.py` | Python | 5 clientes sensor con reconexión automática |
| `OperatorDashboard.java` | Java | Panel de control con GUI Swing y login |
| `operator_dashboard.py` | Python | Panel de control alternativo con Tkinter |
| `AuthServer.java` | Java | Servicio externo de autenticación (puerto 9000) |
| `users.json` | JSON | Base de usuarios y roles |
| `Dockerfile` | — | Imagen Docker para el servidor C |
| `Dockerfile.auth` | — | Imagen Docker para el AuthServer Java |
| `docker-compose.yml` | — | Orquesta ambos contenedores en red interna |

---

## Protocolo de aplicación

Protocolo de texto propio, diseñado por el equipo, sobre TCP. Todos los mensajes terminan en `\n` como delimitador.

### Comandos

| Comando | Formato | Respuesta exitosa | Error |
|---|---|---|---|
| Registrar sensor | `REGISTER\|id\|tipo` | `OK\|REGISTERED` | `ERR\|MAX_SENSORS_REACHED` |
| Enviar medición | `DATA\|id\|tipo\|valor` | `OK\|ACK` | `ERR\|FORMATO_INVALIDO` |
| Consultar estado | `GET_STATE` | `id:tipo:valor:HH:MM:SS;...` | `EMPTY` |
| Consultar alertas | `GET_ALERTS` | `descripcion\n...` | `NONE` |
| Iniciar sesión | `LOGIN\|usuario\|clave` | `OK\|rol` | `ERR\|UNAUTHORIZED` |
| Suscribirse a alertas push | `SUBSCRIBE` | `OK\|SUBSCRIBED` + mensajes push | — |
| Dashboard web | `GET / HTTP/1.1` | HTML con Basic Auth | `401` / `403` |

### Notificaciones push

Cuando un sensor supera un umbral, el servidor envía automáticamente a todos los operadores suscritos:

```
ALERT|<descripcion>\n
```

### Tipos de sensor y umbrales

| Tipo | Descripción | Unidad | Umbral de alerta |
|---|---|---|---|
| `TEMP` | Temperatura | °C | > 80 |
| `VIBR` | Vibración mecánica | mm/s | > 10 |
| `ENER` | Consumo energético | kWh | > 18 |
| `HUM` | Humedad | % | > 90 |
| `STAT` | Estado operativo | — | contiene "Falla" |

### Ejemplo de sesión completa

```
# Sensor se registra y envía datos
→ REGISTER|TEMP_01|TEMP
← OK|REGISTERED

→ DATA|TEMP_01|TEMP|85.3
← OK|ACK
  (servidor genera: ALERT|TEMP_ALTA en TEMP_01: 85.30 C)

# Operador consulta estado
→ GET_STATE
← TEMP_01:TEMP:85.3:14:22:05;VIBR_02:VIBR:2.1:14:22:04;

# Operador se suscribe a alertas push
→ SUBSCRIBE
← OK|SUBSCRIBED
← ALERT|TEMP_ALTA en TEMP_01: 85.30 C    ← llega automáticamente

# Login desde el cliente Java
→ LOGIN|operador|op456
← OK|operador
```

### Justificación de TCP sobre UDP

El sistema usa `SOCK_STREAM` (TCP) en todos los componentes. Las razones concretas:

- El protocolo tiene **comandos con respuesta** (`DATA → OK|ACK`, `REGISTER → OK|REGISTERED`), lo que requiere una conversación confiable de ida y vuelta.
- Las mediciones se almacenan en RAM en el servidor. Una pérdida silenciosa de paquetes crearía huecos en el historial sin que nadie lo supiera.
- Las alertas push **deben llegar garantizadas** a los operadores. UDP no ofrece esa garantía.
- Con 5 sensores enviando cada 3 segundos, el overhead de TCP es despreciable.

El tipo de socket se define en una línea de `server.c`:

```c
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
//                              ^^^^^^^^^^^
//                              SOCK_STREAM = TCP
//                              SOCK_DGRAM  = UDP (no utilizado)
```

---

## Servicio de autenticación

El servidor **no almacena usuarios localmente**. Cuando recibe `LOGIN|usuario|clave`, consulta el `AuthServer.java` en el puerto 9000 por nombre de dominio (nunca por IP).

El AuthServer lee los usuarios de `users.json`:

```json
{
    "admin":    { "password": "admin123", "role": "admin"    },
    "operador": { "password": "op456",    "role": "operador" },
    "viewer":   { "password": "view789",  "role": "viewer"   }
}
```

El archivo puede editarse sin reconstruir la imagen Docker — está montado como volumen.

---

## Requisitos

### Servidor C
- GCC 10+ con soporte pthread
- Linux o macOS para compilación nativa (o Docker)

### Clientes Python
- Python 3.8+
- Tkinter: `sudo apt install python3-tk` (Linux) — incluido en macOS

### Cliente Java y AuthServer
- JDK 11+ (`sudo apt install default-jdk` en Linux, `brew install openjdk` en macOS)
- No requiere dependencias externas — solo biblioteca estándar Java

### Docker
- Docker Desktop (Windows/macOS) o Docker Engine (Linux)
- Docker Compose

---

## Ejecución local (desarrollo)

Abrir **cuatro terminales** en la carpeta del proyecto.

### Terminal 1 — AuthServer (debe iniciarse primero)

```bash
javac AuthServer.java
java AuthServer
```

Salida esperada:
```
[Auth] Usuarios cargados: [viewer, admin, operador]
=== Servicio de Autenticacion IoT ===
Puerto : 9000
Esperando conexiones...
```

### Terminal 2 — Servidor C

```bash
gcc -Wall -o server server.c -lpthread
AUTH_HOST=localhost ./server 8080 logs.txt
```

Salida esperada:
```
=== Servidor IoT iniciado en puerto 8080 ===
    Logs -> logs.txt
```

### Terminal 3 — Sensores simulados

```bash
python3 iot_clients.py --host localhost
```

Salida esperada:
```
=== Iniciando 5 sensores → localhost:8080 ===

[TEMP_01] Conectado a 127.0.0.1:8080
[TEMP_01] REGISTER → OK|REGISTERED
[TEMP_01] TEMP=45.32 → OK|ACK
...
```

### Terminal 4 — Panel de operador (elegir uno)

**Java (recomendado):**
```bash
javac OperatorDashboard.java
java OperatorDashboard localhost
```

**Python:**
```bash
python3 operator_dashboard.py --host localhost
```

Aparece una pantalla de login. Ingresar con cualquiera de los usuarios de `users.json`.

### Interfaz web

Abrir en el navegador: `http://localhost:8080`

El browser muestra un popup de usuario y contraseña. Ingresar con cualquiera de los usuarios de `users.json`. Una vez autenticado, la página muestra el dashboard con tabla de sensores y alertas, y se recarga automáticamente cada 5 segundos.

Para cerrar sesión: click en el botón **"Cerrar sesion"** en la barra superior. El browser descarta las credenciales y después de 1 segundo redirige al inicio de sesión.

### Probar alertas manualmente

```bash
python3 -c "
import socket
s = socket.socket()
s.connect(('localhost', 8080))
s.send(b'DATA|TEST_99|TEMP|99.5\n')
print(s.recv(256).decode())
s.close()
"
```

El servidor detectará que 99.5°C > 80°C y enviará una alerta push a todos los operadores suscritos.

---

## Despliegue en AWS con Docker

### 1. Crear instancia EC2

1. Lanzar **Ubuntu 22.04 LTS** (t2.micro para Free Tier).
2. En **Security Groups**, abrir los puertos de entrada:
   - TCP 22 — SSH
   - TCP 8080 — servidor IoT y HTTP
3. Guardar el archivo `.pem` de la llave.

### 2. Conectarse a la EC2

```bash
chmod 400 proyecto1-iot-eafit.pem
ssh -i "proyecto1-iot-eafit.pem" ubuntu@<IP-publica>
```

### 3. Instalar Docker en la EC2

```bash
sudo apt update
sudo apt install -y docker.io docker-compose-plugin
sudo systemctl start docker
sudo usermod -aG docker ubuntu
# Cerrar sesión SSH y reconectar para aplicar el grupo
```

### 4. Subir el código

```bash
# Opción A: desde la máquina local
scp -i "proyecto1-iot-eafit.pem" -r . ubuntu@<IP>:~/proyecto/

# Opción B: clonar el repositorio directamente en la EC2
git clone <url-del-repositorio>
cd <nombre-del-repositorio>
```

### 5. Levantar el sistema con Docker Compose

```bash
cd ~/proyecto

# Construir y levantar ambos contenedores
docker compose up --build -d

# Ver logs de ambos servicios en tiempo real
docker compose logs -f

# Solo logs del servidor IoT
docker compose logs -f iot-server

# Solo logs del AuthServer
docker compose logs -f auth-service
```

### 6. Conectar clientes al servidor en la nube

```bash
# Sensores
python3 iot_clients.py --host eafit-internet-proyecto1.work.gd

# Dashboard Java
java OperatorDashboard eafit-internet-proyecto1.work.gd 8080

# Dashboard Python
python3 operator_dashboard.py --host eafit-internet-proyecto1.work.gd

# Interfaz web
# Abrir en el browser: http://eafit-internet-proyecto1.work.gd:8080
```