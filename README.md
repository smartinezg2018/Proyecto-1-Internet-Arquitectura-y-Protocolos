# Sistema Distribuido de Monitoreo de Sensores IoT

Proyecto 1 — Internet: Arquitectura y Protocolos  
Universidad EAFIT

Sistema de monitoreo IoT distribuido desplegado en AWS, compuesto por un servidor central en C, sensores simulados en Python y un panel de control para operadores en Java y Python.

---

## Arquitectura del sistema

```
                        AWS Route 53
                    (DNS: apidominio.proyecto1-iot-eafit.org)
                               │
                    ┌──────────▼──────────┐
                    │     AWS EC2         │
                    │  ┌──────────────┐   │
                    │  │    Docker    │   │
                    │  │  server.c    │   │
                    │  │  puerto 8080 │   │
                    │  └──────────────┘   │
                    └─────────────────────┘
                          │         │
              ┌───────────┘         └───────────────┐
              │                                     │
   ┌──────────▼───────────┐            ┌────────────▼────────────┐
   │  Sensores IoT        │            │  Operadores del sistema │
   │  iot_clients.py      │            │  OperatorDashboard.java │
   │  5 sensores:         │            │  operator_dashboard.py  │
   │  TEMP, VIBR, ENER,   │            │  Interfaz web (browser) │
   │  HUM, STAT           │            └─────────────────────────┘
   └──────────────────────┘
```

### Componentes

| Componente | Lenguaje | Rol |
|---|---|---|
| `server.c` | C | Servidor central. Recibe datos, detecta alertas, sirve HTTP |
| `iot_simulator.py` | Python | Modelo de simulación de sensores (valores realistas) |
| `iot_clients.py` | Python | Clientes sensor: 5 hilos que envían mediciones periódicas |
| `OperatorDashboard.java` | Java | Panel de control con GUI Swing para operadores |
| `operator_dashboard.py` | Python | Panel de control alternativo con GUI Tkinter |

---

## Protocolo de aplicación

El sistema utiliza un protocolo de capa de aplicación **basado en texto, diseñado por el equipo**, sobre TCP. Todos los mensajes terminan en `\n` como delimitador.

### Comandos disponibles

| Comando | Formato | Respuesta exitosa | Respuesta de error |
|---|---|---|---|
| Registrar sensor | `REGISTER\|<id>\|<tipo>` | `OK\|REGISTERED` | `ERR\|MAX_SENSORS_REACHED` |
| Enviar medición | `DATA\|<id>\|<tipo>\|<valor>` | `OK\|ACK` | `ERR\|FORMATO_INVALIDO` |
| Consultar estado | `GET_STATE` | `id:tipo:valor:HH:mm:ss;...` | `EMPTY` |
| Consultar alertas | `GET_ALERTS` | `descripcion\n...` | `NONE` |
| Suscribirse a alertas push | `SUBSCRIBE` | `OK\|SUBSCRIBED` + mensajes push | — |
| Iniciar sesión | `LOGIN\|<usuario>\|<clave>` | `OK\|<rol>` | `ERR\|UNAUTHORIZED` |
| Interfaz web | `GET / HTTP/1.1` | Página HTML con estado | `404 Not Found` |

### Notificaciones push

Cuando un sensor supera un umbral, el servidor envía automáticamente a todos los operadores suscritos:

```
ALERT|<descripcion>\n
```

### Tipos de sensor y umbrales de alerta

| Tipo | Descripción | Unidad | Umbral de alerta |
|---|---|---|---|
| `TEMP` | Temperatura | °C | > 80 |
| `VIBR` | Vibración mecánica | mm/s | > 10 |
| `ENER` | Consumo energético | kWh | > 18 |
| `HUM` | Humedad | % | > 90 |
| `STAT` | Estado operativo | — | contiene "Falla" |

### Ejemplo de sesión

```
Cliente → Servidor:   REGISTER|TEMP_01|TEMP\n
Servidor → Cliente:   OK|REGISTERED\n

Cliente → Servidor:   DATA|TEMP_01|TEMP|46.39\n
Servidor → Cliente:   OK|ACK\n

Cliente → Servidor:   GET_STATE\n
Servidor → Cliente:   TEMP_01:TEMP:46.39:13:22:05;VIBR_02:VIBR:2.4:13:22:05;\n

Cliente → Servidor:   SUBSCRIBE\n
Servidor → Cliente:   OK|SUBSCRIBED\n
Servidor → Cliente:   ALERT|TEMP_ALTA en TEMP_01: 82.5 C (max 80)\n   ← push automático
```

### Justificación de TCP sobre UDP

El sistema usa `SOCK_STREAM` (TCP) en todos los componentes. Las razones:

- El protocolo tiene **comandos con respuesta** (`DATA → OK|ACK`), lo que requiere una conversación confiable de ida y vuelta.
- Los datos de sensores se almacenan en memoria en el servidor. Una medición perdida silenciosamente genera un hueco en el historial sin que nadie lo sepa.
- Las alertas push deben llegar **garantizadas** a los operadores. Con UDP no hay forma de saber si llegaron.
- UDP sería apropiado con miles de sensores enviando lecturas en intervalos de milisegundos donde una pérdida ocasional es aceptable. Con 5 sensores y lecturas cada 3 segundos, el overhead de TCP es despreciable.

El tipo de socket se define en una sola línea de `server.c`:

```c
int server_fd = socket(AF_INET, SOCK_STREAM, 0);
//                              ^^^^^^^^^^^
//                              SOCK_STREAM = TCP
//                              SOCK_DGRAM  = UDP (no utilizado)
```

---

## Requisitos

### Servidor
- GCC 10+ con soporte para pthreads
- Linux o macOS (para compilación nativa) — o Docker

### Clientes Python
- Python 3.8+
- Tkinter: `sudo apt install python3-tk` (Linux) — incluido en macOS

### Cliente Java
- JDK 11+ (`sudo apt install default-jdk` en Linux, `brew install openjdk` en macOS)
- No requiere dependencias externas (solo biblioteca estándar Java)

### Docker
- Docker Desktop (Windows/macOS) o Docker Engine (Linux)

---

## Ejecución local (desarrollo)

Abrir **tres terminales** en la carpeta del proyecto.

### Terminal 1 — Compilar y ejecutar el servidor

```bash
gcc -Wall -o server server.c -lpthread
./server 8080 logs.txt
```

Salida esperada:
```
=== Servidor IoT iniciado en puerto 8080 ===
    Logs → logs.txt
```

### Terminal 2 — Lanzar los 5 sensores simulados

```bash
python3 iot_clients.py --host localhost
```

Salida esperada:
```
=== Iniciando 5 sensores → localhost:8080 ===

[TEMP_01] Conectado a 127.0.0.1:8080
[TEMP_01] REGISTER → OK|REGISTERED
[TEMP_01] TEMP=46.39 → OK|ACK
[VIBR_02] VIBR=2.40 → OK|ACK
...
```

### Terminal 3 — Panel de operador (elegir uno)

**Java:**
```bash
javac OperatorDashboard.java
java OperatorDashboard localhost
```

**Python:**
```bash
python3 operator_dashboard.py --host localhost
```

### Interfaz web

Abrir en el navegador: [http://localhost:8080](http://localhost:8080)

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

---

## Despliegue en AWS

### 1. Crear la instancia EC2

1. En la consola AWS, lanzar una instancia **Ubuntu 22.04 LTS** (t2.micro para Free Tier).
2. En **Security Groups**, abrir los puertos de entrada:
   - TCP 22 — SSH
   - TCP 8080 — servidor IoT y HTTP
3. Guardar el archivo `.pem` de la llave privada.

### 2. Conectarse a la instancia

```bash
chmod 400 proyecto1-iot-eafit.pem
ssh -i "proyecto1-iot-eafit.pem" ubuntu@<IP-publica-EC2>
```

### 3. Instalar Docker en la EC2

```bash
sudo apt update
sudo apt install -y docker.io
sudo systemctl start docker
sudo usermod -aG docker ubuntu
# Cerrar sesión y volver a conectar para que aplique el grupo
```

### 4. Subir el código a la EC2

Desde la máquina local:
```bash
scp -i "proyecto1-iot-eafit.pem" server.c Dockerfile ubuntu@<IP>:~/proyecto/
```

O clonar directamente desde el repositorio:
```bash
git clone <url-del-repositorio>
cd <nombre-del-repositorio>
```

### 5. Construir y ejecutar el contenedor Docker

```bash
cd ~/proyecto

# Construir la imagen
docker build -t iot-server .

# Ejecutar el contenedor
docker run -d \
  --name iot-monitor \
  -p 8080:8080 \
  -v $(pwd)/logs:/app/logs \
  iot-server

# Ver logs en tiempo real
docker logs -f iot-monitor
```

### 6. Configurar DNS

Usando un proveedor DNS gratuito (FreeDNS, No-IP, etc.):

1. Crear una cuenta y registrar un subdominio, por ejemplo `apidominio.proyecto1-iot-eafit.org`.
2. Crear un registro de tipo **A** apuntando a la IP pública de la EC2.
3. Verificar la resolución:

```bash
nslookup apidominio.proyecto1-iot-eafit.org
ping apidominio.proyecto1-iot-eafit.org
```

### 7. Conectar los clientes al servidor en la nube

```bash
# Sensores
python3 iot_clients.py --host apidominio.proyecto1-iot-eafit.org

# Dashboard Java
java OperatorDashboard apidominio.proyecto1-iot-eafit.org 8080

# Dashboard Python
python3 operator_dashboard.py --host apidominio.proyecto1-iot-eafit.org

# Interfaz web
# Abrir en el navegador: http://apidominio.proyecto1-iot-eafit.org:8080
```
