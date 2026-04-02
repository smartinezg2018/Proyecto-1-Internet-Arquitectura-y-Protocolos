/**
 * AuthServer.java — Servicio externo de autenticación IoT
 *
 * Escucha en el puerto 9000 y valida credenciales consultando users.json.
 * El servidor central (server.c) se comunica con este servicio cuando
 * recibe un comando LOGIN, sin almacenar usuarios localmente.
 *
 * Protocolo (mismo canal TCP, texto plano):
 *   Entrada:  LOGIN|<usuario>|<clave>
 *   Salida:   OK|<rol>          si las credenciales son válidas
 *             ERR|UNAUTHORIZED  si son incorrectas
 *             ERR|FORMATO       si el mensaje no tiene el formato esperado
 *
 * Compile:  javac AuthServer.java
 * Run:      java AuthServer
 *           java AuthServer 9000 users.json
 *
 * No requiere dependencias externas — solo JDK 11+
 */

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.*;

public class AuthServer {

    static final int    DEFAULT_PORT       = 9000;
    static final String DEFAULT_USERS_FILE = "users.json";

    // Mapa usuario → {clave, rol} cargado desde users.json
    // Se recarga automáticamente si el archivo cambia
    static final Map<String, String[]> users = new ConcurrentHashMap<>();
    static volatile long usersFileLastModified = 0;
    static String usersFilePath;

    // ── Cargar usuarios desde users.json ──────────────────────────
    //
    // Formato del archivo:
    // {
    //   "admin":    { "password": "admin123",  "role": "admin"    },
    //   "operador": { "password": "op456",     "role": "operador" },
    //   "viewer":   { "password": "view789",   "role": "viewer"   }
    // }
    //
    // Parser manual para no depender de librerías JSON externas.
    //
    static synchronized void loadUsers(String filepath) {
        File f = new File(filepath);
        if (!f.exists()) {
            System.err.println("[Auth] Advertencia: " + filepath + " no encontrado. Usando usuarios por defecto.");
            // Usuarios por defecto si no hay archivo
            users.put("admin",    new String[]{"admin123",  "admin"});
            users.put("operador", new String[]{"op456",     "operador"});
            users.put("viewer",   new String[]{"view789",   "viewer"});
            return;
        }

        try {
            String content = new String(Files.readAllBytes(f.toPath()));
            Map<String, String[]> loaded = new HashMap<>();

            // Parsear cada bloque "usuario": { "password": "...", "role": "..." }
            // Buscamos patrones: "clave": "valor"
            int pos = 0;
            while (pos < content.length()) {
                // Encontrar nombre de usuario (primera clave del objeto externo)
                int userStart = content.indexOf('"', pos);
                if (userStart == -1) break;
                int userEnd = content.indexOf('"', userStart + 1);
                if (userEnd == -1) break;
                String username = content.substring(userStart + 1, userEnd);

                // Saltar si es "password" o "role" (son claves internas)
                if (username.equals("password") || username.equals("role")) {
                    pos = userEnd + 1;
                    continue;
                }

                // Buscar bloque interno { ... }
                int blockStart = content.indexOf('{', userEnd);
                int blockEnd   = content.indexOf('}', blockStart);
                if (blockStart == -1 || blockEnd == -1) break;
                String block = content.substring(blockStart, blockEnd + 1);

                String password = extractJsonValue(block, "password");
                String role     = extractJsonValue(block, "role");

                if (password != null && role != null) {
                    loaded.put(username, new String[]{password, role});
                }
                pos = blockEnd + 1;
            }

            if (!loaded.isEmpty()) {
                users.clear();
                users.putAll(loaded);
                usersFileLastModified = f.lastModified();
                System.out.println("[Auth] Usuarios cargados: " + users.keySet());
            }

        } catch (IOException e) {
            System.err.println("[Auth] Error leyendo " + filepath + ": " + e.getMessage());
        }
    }

    // Extrae el valor de "key": "value" dentro de un bloque JSON simple
    static String extractJsonValue(String block, String key) {
        String search = "\"" + key + "\"";
        int idx = block.indexOf(search);
        if (idx == -1) return null;
        int colon = block.indexOf(':', idx + search.length());
        if (colon == -1) return null;
        int valStart = block.indexOf('"', colon + 1);
        if (valStart == -1) return null;
        int valEnd = block.indexOf('"', valStart + 1);
        if (valEnd == -1) return null;
        return block.substring(valStart + 1, valEnd);
    }

    // Recarga el archivo si fue modificado (hot-reload sin reiniciar)
    static void reloadIfChanged() {
        File f = new File(usersFilePath);
        if (f.exists() && f.lastModified() > usersFileLastModified) {
            System.out.println("[Auth] Detectado cambio en " + usersFilePath + ", recargando...");
            loadUsers(usersFilePath);
        }
    }

    // ── Validar credenciales ───────────────────────────────────────
    static String authenticate(String username, String password) {
        reloadIfChanged();
        String[] data = users.get(username);
        if (data == null) return "ERR|UNAUTHORIZED";
        if (!data[0].equals(password)) return "ERR|UNAUTHORIZED";
        return "OK|" + data[1];   // OK|admin, OK|operador, etc.
    }

    // ── Hilo por conexión ──────────────────────────────────────────
    static void handleConnection(Socket sock) {
        String clientIp = sock.getInetAddress().getHostAddress();
        int    clientPort = sock.getPort();

        try (
            InputStream  rawIn  = sock.getInputStream();
            OutputStream rawOut = sock.getOutputStream()
        ) {
            // Leer hasta \n (mismo método que los otros clientes del sistema)
            StringBuilder sb = new StringBuilder();
            int c;
            while ((c = rawIn.read()) != -1) {
                if (c == '\n') break;
                if (c != '\r') sb.append((char) c);
            }

            String msg = sb.toString().trim();
            String response;

            if (msg.startsWith("LOGIN|")) {
                // Formato: LOGIN|<usuario>|<clave>
                String[] parts = msg.split("\\|", 3);
                if (parts.length == 3) {
                    response = authenticate(parts[1], parts[2]);
                } else {
                    response = "ERR|FORMATO";
                }
            } else {
                response = "ERR|FORMATO";
            }

            rawOut.write((response + "\n").getBytes("UTF-8"));
            rawOut.flush();

            System.out.printf("[Auth] %s:%d | RX: %s | TX: %s%n",
                clientIp, clientPort, msg, response);

        } catch (IOException e) {
            System.err.println("[Auth] Error con cliente " + clientIp + ": " + e.getMessage());
        } finally {
            try { sock.close(); } catch (IOException ignored) {}
        }
    }

    // ── main ───────────────────────────────────────────────────────
    public static void main(String[] args) throws IOException {
        int    port      = DEFAULT_PORT;
        String usersFile = DEFAULT_USERS_FILE;

        if (args.length >= 1) {
            try { port = Integer.parseInt(args[0]); }
            catch (NumberFormatException e) { System.err.println("Puerto invalido, usando " + DEFAULT_PORT); }
        }
        if (args.length >= 2) usersFile = args[1];
        usersFilePath = usersFile;

        loadUsers(usersFile);

        // Pool de hilos para manejar múltiples conexiones simultáneas
        ExecutorService pool = Executors.newCachedThreadPool();

        ServerSocket serverSock = new ServerSocket(port);
        serverSock.setReuseAddress(true);

        System.out.println("=== Servicio de Autenticacion IoT ===");
        System.out.println("Puerto : " + port);
        System.out.println("Usuarios: " + usersFile);
        System.out.println("Esperando conexiones...\n");

        // Bucle principal — no termina ante errores individuales
        while (true) {
            try {
                Socket client = serverSock.accept();
                pool.submit(() -> handleConnection(client));
            } catch (IOException e) {
                System.err.println("[Auth] Error aceptando conexion: " + e.getMessage());
            }
        }
    }
}
