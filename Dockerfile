# ── Etapa 1: compilar el servidor en C ────────────────────────────
# Usamos la imagen oficial de GCC que ya trae compilador y librerías
FROM gcc:12 AS builder

WORKDIR /build
COPY server.c .

# Compilamos con -Wall para ver advertencias, -lpthread para los hilos
RUN gcc -Wall -o server server.c -lpthread

# ── Etapa 2: imagen final mínima ───────────────────────────────────
# Copiamos solo el binario a una imagen Debian slim (más liviana)
FROM debian:bookworm-slim

WORKDIR /app

# Copiar el binario compilado desde la etapa anterior
COPY --from=builder /build/server .

# Crear directorio para los logs dentro del contenedor
RUN mkdir -p /app/logs

# Puerto que expone el servidor (mismo que usan los clientes)
EXPOSE 8080

# Comando de inicio: ./server <puerto> <archivo_de_logs>
CMD ["./server", "8080", "logs/server.log"]
