/*
 * server.c — Servidor Central IoT
 * Uso: ./server <puerto> <archivo_de_logs>
 *
 * Protocolo de texto soportado:
 *   REGISTER|<sensor_id>|<tipo>          → OK|REGISTERED o ERR|DUPLICATE
 *   DATA|<sensor_id>|<tipo>|<valor>      → OK|ACK o ERR|ALERT|<detalle>
 *   GET_STATE                            → id:tipo:valor:ts;id:tipo:valor:ts;...
 *   GET_ALERTS                           → alerta1\nalerta2\n... o NONE
 *   LOGIN|<usuario>|<clave>              → OK|<rol> o ERR|UNAUTHORIZED
 *   GET / HTTP/1.1                       → respuesta HTTP con página de estado
 */

#define _POSIX_C_SOURCE 200112L  /* garantiza struct addrinfo en todos los sistemas */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>    /* debe ir antes de socket.h y netdb.h */
#include <sys/socket.h>   /* debe ir antes de netdb.h */
#include <netdb.h>        /* struct addrinfo, getaddrinfo, freeaddrinfo */
#include <arpa/inet.h>
#include <sys/time.h>    /* struct timeval para SO_RCVTIMEO */    /* inet_ntop, htons, INADDR_ANY */

/* ─── Constantes ─────────────────────────────────────────────── */
#define MAX_SENSORS   50
#define MAX_OPERATORS 20
#define MAX_ALERTS    100
#define BUF_SIZE      4096

/* Umbrales para generar alertas automáticas */
#define TEMP_MAX      80.0
#define VIBR_MAX      10.0
#define ENER_MAX      18.0
#define HUM_MAX       90.0

/* ─── Estructuras ─────────────────────────────────────────────── */
typedef struct {
    char id[32];
    char tipo[32];
    char valor[64];
    char unidad[16];
    char timestamp[32];
    int  activo;
} sensor_t;

typedef struct {
    int  socket;
    char ip[INET_ADDRSTRLEN];
    int  port;
} operator_t;

typedef struct {
    int    socket;
    struct sockaddr_in address;
    FILE  *log_file;
} client_ctx_t;

/* ─── Estado global ───────────────────────────────────────────── */
sensor_t   sensors[MAX_SENSORS];
int        sensor_count = 0;
pthread_mutex_t sensors_lock = PTHREAD_MUTEX_INITIALIZER;

operator_t operators[MAX_OPERATORS];
int        operator_count = 0;
pthread_mutex_t operators_lock = PTHREAD_MUTEX_INITIALIZER;

char alerts[MAX_ALERTS][256];
int  alert_count = 0;
pthread_mutex_t alerts_lock = PTHREAD_MUTEX_INITIALIZER;

FILE *g_log = NULL;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* ─── Logging ─────────────────────────────────────────────────── */
void log_event(const char *ip, int port, const char *rx, const char *tx) {
    time_t now = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&now));

    pthread_mutex_lock(&log_lock);
    /* consola */
    printf("[%s] %s:%d | RX: %.80s | TX: %.80s\n", ts, ip, port, rx, tx);
    /* archivo */
    if (g_log) {
        fprintf(g_log, "[%s] %s:%d | RX: %s | TX: %s\n", ts, ip, port, rx, tx);
        fflush(g_log);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ─── Timestamp actual ────────────────────────────────────────── */
void current_ts(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%H:%M:%S", localtime(&now));
}

/* ─── Alerta: guardar y notificar operadores ──────────────────── */
void push_alert(const char *msg) {
    /* guardar en historial */
    pthread_mutex_lock(&alerts_lock);
    if (alert_count < MAX_ALERTS) {
        strncpy(alerts[alert_count], msg, 255);
        alert_count++;
    }
    pthread_mutex_unlock(&alerts_lock);

    /* notificar a todos los operadores conectados */
    char notif[300];
    snprintf(notif, sizeof(notif), "ALERT|%s\n", msg);

    pthread_mutex_lock(&operators_lock);
    for (int i = 0; i < operator_count; i++) {
        if (operators[i].socket > 0) {
            if (send(operators[i].socket, notif, strlen(notif), MSG_NOSIGNAL) < 0) {
                /* operador desconectado: marcarlo como libre */
                operators[i].socket = 0;
            }
        }
    }
    pthread_mutex_unlock(&operators_lock);
}

/* ─── Verificar umbrales y generar alertas ────────────────────── */
void check_thresholds(const char *sensor_id, const char *tipo, const char *valor) {
    char msg[256];
    int alerta = 0;

    /* STAT es texto, no número: alerta solo si indica falla */
    if (strcmp(tipo, "STAT") == 0) {
        if (strstr(valor, "Falla") || strstr(valor, "falla")) {
            snprintf(msg, sizeof(msg), "FALLA en %s: estado=%s", sensor_id, valor);
            alerta = 1;
        }
    } else {
        /* Para el resto de tipos, comparar valor numérico contra umbral */
        double v = atof(valor);
        if (strcmp(tipo, "TEMP") == 0 && v > TEMP_MAX) {
            snprintf(msg, sizeof(msg), "TEMP_ALTA en %s: %.2f C (max %.0f)", sensor_id, v, TEMP_MAX);
            alerta = 1;
        } else if (strcmp(tipo, "VIBR") == 0 && v > VIBR_MAX) {
            snprintf(msg, sizeof(msg), "VIBRACION_ALTA en %s: %.2f mm/s (max %.0f)", sensor_id, v, VIBR_MAX);
            alerta = 1;
        } else if (strcmp(tipo, "ENER") == 0 && v > ENER_MAX) {
            snprintf(msg, sizeof(msg), "CONSUMO_ALTO en %s: %.2f kWh (max %.0f)", sensor_id, v, ENER_MAX);
            alerta = 1;
        } else if (strcmp(tipo, "HUM") == 0 && v > HUM_MAX) {
            snprintf(msg, sizeof(msg), "HUMEDAD_ALTA en %s: %.1f%% (max %.0f)", sensor_id, v, HUM_MAX);
            alerta = 1;
        }
    }

    if (alerta) push_alert(msg);
}

/* ─── Unidad segun tipo de sensor ─────────────────────────────────── */
const char *unidad_de_tipo(const char *tipo) {
    if (strcmp(tipo, "TEMP") == 0) return "C";
    if (strcmp(tipo, "VIBR") == 0) return "mm/s";
    if (strcmp(tipo, "ENER") == 0) return "kWh";
    if (strcmp(tipo, "HUM")  == 0) return "%";
    return "";
}

/* ─── REGISTER ────────────────────────────────────────────────── */
void handle_register(const char *msg, char *resp) {
    /* formato: REGISTER|<id>|<tipo> */
    char tmp[BUF_SIZE];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    strtok(tmp, "|");                  /* "REGISTER" */
    char *id   = strtok(NULL, "|");
    char *tipo = strtok(NULL, "|\r\n");

    if (!id || !tipo) { strcpy(resp, "ERR|FORMATO_INVALIDO"); return; }

    pthread_mutex_lock(&sensors_lock);
    for (int i = 0; i < sensor_count; i++) {
        if (strcmp(sensors[i].id, id) == 0) {
            sensors[i].activo = 1;          /* reconexión */
            pthread_mutex_unlock(&sensors_lock);
            strcpy(resp, "OK|REGISTERED");
            return;
        }
    }
    if (sensor_count < MAX_SENSORS) {
        strncpy(sensors[sensor_count].id,   id,   31);
        strncpy(sensors[sensor_count].tipo, tipo, 31);
        strncpy(sensors[sensor_count].unidad, unidad_de_tipo(tipo), 15);
        strcpy (sensors[sensor_count].valor, "---");
        current_ts(sensors[sensor_count].timestamp, 32);
        sensors[sensor_count].activo = 1;
        sensor_count++;
        pthread_mutex_unlock(&sensors_lock);
        strcpy(resp, "OK|REGISTERED");
    } else {
        pthread_mutex_unlock(&sensors_lock);
        strcpy(resp, "ERR|MAX_SENSORS_REACHED");
    }
}

/* ─── DATA ────────────────────────────────────────────────────── */
void handle_data(const char *msg, char *resp) {
    /* formato: DATA|<id>|<tipo>|<valor> */
    char tmp[BUF_SIZE];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    strtok(tmp, "|");
    char *id    = strtok(NULL, "|");
    char *tipo  = strtok(NULL, "|");
    char *valor = strtok(NULL, "|\r\n");

    if (!id || !tipo || !valor) { strcpy(resp, "ERR|FORMATO_INVALIDO"); return; }

    pthread_mutex_lock(&sensors_lock);
    int found = 0;
    for (int i = 0; i < sensor_count; i++) {
        if (strcmp(sensors[i].id, id) == 0) {
            strncpy(sensors[i].valor, valor, 63);
            current_ts(sensors[i].timestamp, 32);
            found = 1;
            break;
        }
    }
    /* auto-registro si el sensor no hizo REGISTER antes */
    if (!found && sensor_count < MAX_SENSORS) {
        strncpy(sensors[sensor_count].id,    id,    31);
        strncpy(sensors[sensor_count].tipo,  tipo,  31);
        strncpy(sensors[sensor_count].unidad, unidad_de_tipo(tipo), 15);
        strncpy(sensors[sensor_count].valor, valor, 63);
        current_ts(sensors[sensor_count].timestamp, 32);
        sensors[sensor_count].activo = 1;
        sensor_count++;
    }
    pthread_mutex_unlock(&sensors_lock);

    check_thresholds(id, tipo, valor);
    strcpy(resp, "OK|ACK");
}

/* ─── GET_STATE ───────────────────────────────────────────────── */
void handle_get_state(char *resp, size_t sz) {
    pthread_mutex_lock(&sensors_lock);
    resp[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < sensor_count; i++) {
        char entry[128];
        int n = snprintf(entry, sizeof(entry), "%s:%s:%s:%s;",
                         sensors[i].id, sensors[i].tipo,
                         sensors[i].valor, sensors[i].timestamp);
        if (used + n < sz - 1) {
            strcat(resp, entry);
            used += n;
        }
    }
    if (used == 0) strcpy(resp, "EMPTY");
    pthread_mutex_unlock(&sensors_lock);
}

/* ─── GET_ALERTS ──────────────────────────────────────────────── */
void handle_get_alerts(char *resp, size_t sz) {
    pthread_mutex_lock(&alerts_lock);
    if (alert_count == 0) {
        strcpy(resp, "NONE");
    } else {
        resp[0] = '\0';
        size_t used = 0;
        /* devolver las últimas 10 alertas */
        int start = (alert_count > 10) ? alert_count - 10 : 0;
        for (int i = start; i < alert_count; i++) {
            size_t len = strlen(alerts[i]);
            if (used + len + 2 < sz) {
                strcat(resp, alerts[i]);
                strcat(resp, "\n");
                used += len + 1;
            }
        }
    }
    pthread_mutex_unlock(&alerts_lock);
}

/* ─── LOGIN (consulta servicio auth externo) ──────────────────── */
void handle_login(const char *msg, char *resp) {
    /*
     * Consulta a un servicio de auth externo en auth.proyecto1-iot-eafit.org:9000
     * Protocolo interno: AUTH|<usuario>|<clave>  →  OK|<rol>  o  ERR|UNAUTHORIZED
     * Si el servicio no está disponible, deniega el acceso.
     */
    char tmp[BUF_SIZE];
    strncpy(tmp, msg, sizeof(tmp) - 1);

    /* Resolver nombre de dominio del servicio de auth */
    /* AUTH_HOST puede setearse como variable de entorno.
     * Prioridad: variable de entorno > valor por defecto.
     * En docker-compose se define como "auth-service" (nombre del contenedor).
     * En desarrollo local se puede usar "localhost". */
    const char *auth_host_env = getenv("AUTH_HOST");
    const char *auth_host = auth_host_env ? auth_host_env : "auth-service";
    const char *auth_port = "9000";

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(auth_host, auth_port, &hints, &res) != 0) {
        /* Si no se puede resolver el DNS, denegar de forma segura */
        strcpy(resp, "ERR|AUTH_SERVICE_UNAVAILABLE");
        return;
    }

    int auth_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (auth_fd < 0 || connect(auth_fd, res->ai_addr, res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        if (auth_fd >= 0) close(auth_fd);
        strcpy(resp, "ERR|AUTH_SERVICE_UNAVAILABLE");
        return;
    }
    freeaddrinfo(res);

    /* Timeout de 5s en recv para evitar que el hilo se bloquee indefinidamente
     * si el AuthServer no responde (p.ej. se cayó o la conexión se perdió) */
    struct timeval tv = {5, 0};   /* 5 segundos */
    setsockopt(auth_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Reenviar el mensaje LOGIN con \n como delimitador */
    char msg_nl[BUF_SIZE + 2];
    snprintf(msg_nl, sizeof(msg_nl), "%s\n", tmp);
    send(auth_fd, msg_nl, strlen(msg_nl), 0);

    /* Leer respuesta hasta \n o timeout */
    char auth_resp[256] = {0};
    int  ar_len = 0;
    char ch;
    while (ar_len < (int)sizeof(auth_resp) - 1) {
        int r = recv(auth_fd, &ch, 1, 0);
        if (r <= 0) break;          /* timeout, error o conexión cerrada */
        if (ch == '\n') break;
        if (ch != '\r') auth_resp[ar_len++] = ch;
    }
    close(auth_fd);

    if (ar_len == 0) {
        strcpy(resp, "ERR|AUTH_TIMEOUT");
        return;
    }
    strncpy(resp, auth_resp, BUF_SIZE - 1);
}

/* ─── Base64 decode (para HTTP Basic Auth) ────────────────────── */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64_decode(const char *in, char *out, int out_sz) {
    int len = strlen(in), out_len = 0;
    for (int i = 0; i < len - 3 && out_len < out_sz - 1; i += 4) {
        int v = 0;
        for (int j = 0; j < 4; j++) {
            char *p = strchr(b64_table, in[i+j]);
            v = (v << 6) | (p ? (int)(p - b64_table) : 0);
        }
        if (out_len < out_sz - 1) out[out_len++] = (v >> 16) & 0xFF;
        if (in[i+2] != '=' && out_len < out_sz - 1) out[out_len++] = (v >>  8) & 0xFF;
        if (in[i+3] != '=' && out_len < out_sz - 1) out[out_len++] = (v >>  0) & 0xFF;
    }
    out[out_len] = '\0';
    return out_len;
}

/* ─── HTTP 401 Unauthorized ───────────────────────────────────── */
void http_send_401(int sock) {
    const char *body =
        "<!DOCTYPE html><html><body>"
        "<h2>401 - Acceso no autorizado</h2>"
        "<p>Se requieren credenciales validas para acceder al monitor IoT.</p>"
        "</body></html>";
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"IoT Monitor\"\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", strlen(body));
    send(sock, header, strlen(header), 0);
    send(sock, body, strlen(body), 0);
}

/* ─── HTTP 403 Forbidden ──────────────────────────────────────── */
void http_send_403(int sock) {
    const char *body =
        "<!DOCTYPE html><html><body>"
        "<h2>403 - Credenciales invalidas</h2>"
        "<p>Usuario o contrasena incorrectos.</p>"
        "</body></html>";
    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 403 Forbidden\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n", strlen(body));
    send(sock, header, strlen(header), 0);
    send(sock, body, strlen(body), 0);
}

/* ─── HTTP básico con autenticación ───────────────────────────── */
/*
 * Implementa HTTP Basic Authentication (RFC 7617).
 * Flujo:
 *   1. Browser pide GET /  sin credenciales
 *   2. Servidor responde 401 con WWW-Authenticate: Basic
 *   3. Browser muestra popup nativo de usuario/clave
 *   4. Browser reenvía GET / con Authorization: Basic <base64(user:pass)>
 *   5. Servidor decodifica base64, consulta auth externo via handle_login
 *   6. Si OK  → sirve la pagina de metricas (200)
 *      Si ERR → responde 403 Forbidden
 */
void handle_http(int sock, const char *request, const char *ip, int port) {
    /* Buscar header Authorization en el request completo */
    const char *auth_header = strstr(request, "Authorization: Basic ");
    if (!auth_header) {
        http_send_401(sock);
        log_event(ip, port, "GET / HTTP/1.1", "401 Unauthorized");
        return;
    }

    /* Extraer el token base64 (termina en \r o \n o espacio) */
    auth_header += strlen("Authorization: Basic ");
    char b64_token[512] = {0};
    int i = 0;
    while (auth_header[i] && auth_header[i] != '\r' && auth_header[i] != '\n'
           && i < (int)sizeof(b64_token) - 1) {
        b64_token[i] = auth_header[i];
        i++;
    }

    /* Decodificar base64 → "usuario:clave" */
    char credentials[256] = {0};
    b64_decode(b64_token, credentials, sizeof(credentials));

    /* Separar usuario y clave en el primer ":" */
    char *sep = strchr(credentials, ':');
    if (!sep) { http_send_403(sock); return; }
    *sep = '\0';
    char *usuario = credentials;
    char *clave   = sep + 1;

    /* Consultar servicio de autenticación externo */
    char login_msg[512], auth_resp[256];
    snprintf(login_msg, sizeof(login_msg), "LOGIN|%s|%s", usuario, clave);
    handle_login(login_msg, auth_resp);

    if (strncmp(auth_resp, "OK", 2) != 0) {
        http_send_403(sock);
        log_event(ip, port, "GET / HTTP/1.1", "403 Forbidden");
        return;
    }

    /* Credenciales válidas: extraer rol de la respuesta "OK|<rol>" */
    char rol[64] = "operador";
    char *pipe = strchr(auth_resp, '|');
    if (pipe) strncpy(rol, pipe + 1, sizeof(rol) - 1);

    /* Construir tabla de sensores */
    char table_rows[BUF_SIZE] = {0};
    pthread_mutex_lock(&sensors_lock);
    for (int j = 0; j < sensor_count; j++) {
        char row[256];
        snprintf(row, sizeof(row),
            "<tr><td>%s</td><td>%s</td><td>%s %s</td><td>%s</td></tr>\n",
            sensors[j].id, sensors[j].tipo,
            sensors[j].valor, sensors[j].unidad,
            sensors[j].timestamp);
        strncat(table_rows, row, sizeof(table_rows) - strlen(table_rows) - 1);
    }
    pthread_mutex_unlock(&sensors_lock);

    char body[BUF_SIZE * 2];
    int body_len = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5'>"
        "<title>IoT Monitor</title>"
        "<style>body{font-family:sans-serif;margin:2em;background:#1e1e2e;color:#cdd6f4}"
        "table{border-collapse:collapse;width:100%%}"
        "th,td{border:1px solid #45475a;padding:10px;text-align:left}"
        "th{background:#313244}h1{color:#cdd6f4}"
        ".badge{background:#313244;padding:4px 10px;border-radius:4px;font-size:0.85em}"
        "</style></head><body>"
        "<h1>Sistema de Monitoreo IoT</h1>"
        "<p>Usuario: <span class='badge'>%s</span> "
        "Rol: <span class='badge'>%s</span> "
        "Sensores: <strong>%d</strong> "
        "<small style='color:#6c7086'>(recarga automatica cada 5s)</small></p>"
        "<table><tr><th>ID</th><th>Tipo</th><th>Valor</th><th>Ultima lectura</th></tr>"
        "%s"
        "</table></body></html>",
        usuario, rol, sensor_count, table_rows);

    char header[512];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", body_len);

    send(sock, header, strlen(header), 0);
    send(sock, body, body_len, 0);

    log_event(ip, port, "GET / HTTP/1.1", "200 OK");
}

/* ─── Registro de operador ────────────────────────────────────── */
void register_operator(int sock, const char *ip, int port) {
    pthread_mutex_lock(&operators_lock);
    /* buscar slot libre (socket == 0) */
    for (int i = 0; i < MAX_OPERATORS; i++) {
        if (operators[i].socket == 0) {
            operators[i].socket = sock;
            operators[i].port   = port;
            strncpy(operators[i].ip, ip, INET_ADDRSTRLEN - 1);
            if (i >= operator_count) operator_count = i + 1;
            pthread_mutex_unlock(&operators_lock);
            return;
        }
    }
    pthread_mutex_unlock(&operators_lock);
}

void unregister_operator(int sock) {
    pthread_mutex_lock(&operators_lock);
    for (int i = 0; i < operator_count; i++) {
        if (operators[i].socket == sock) {
            operators[i].socket = 0;
            break;
        }
    }
    pthread_mutex_unlock(&operators_lock);
}

/* ─── Hilo por cliente ────────────────────────────────────────── */
void *handle_client(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    char  buffer[BUF_SIZE];
    char  response[BUF_SIZE];
    char  ip[INET_ADDRSTRLEN];
    int   port;

    inet_ntop(AF_INET, &ctx->address.sin_addr, ip, sizeof(ip));
    port = ntohs(ctx->address.sin_port);

    int is_operator = 0;
    ssize_t n;

    while ((n = recv(ctx->socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[n] = '\0';
        response[0] = '\0';

        /* ── HTTP ── */
        if (strncmp(buffer, "GET /", 5) == 0 || strncmp(buffer, "GET H", 5) == 0) {
            handle_http(ctx->socket, buffer, ip, port);
            break;  /* HTTP/1.0 cierra tras la respuesta */
        }
        /* ── REGISTER ── */
        else if (strncmp(buffer, "REGISTER", 8) == 0) {
            handle_register(buffer, response);
        }
        /* ── DATA ── */
        else if (strncmp(buffer, "DATA", 4) == 0) {
            handle_data(buffer, response);
        }
        /* ── GET_STATE ── */
        else if (strncmp(buffer, "GET_STATE", 9) == 0) {
            handle_get_state(response, sizeof(response));
        }
        /* ── GET_ALERTS ── */
        else if (strncmp(buffer, "GET_ALERTS", 10) == 0) {
            handle_get_alerts(response, sizeof(response));
        }
        /* ── LOGIN ── */
        else if (strncmp(buffer, "LOGIN", 5) == 0) {
            handle_login(buffer, response);
        }
        /* ── SUBSCRIBE (operador pide alertas push) ── */
        else if (strncmp(buffer, "SUBSCRIBE", 9) == 0) {
            register_operator(ctx->socket, ip, port);
            is_operator = 1;
            strcpy(response, "OK|SUBSCRIBED");
        }
        else {
            strcpy(response, "ERR|COMANDO_DESCONOCIDO");
        }

        if (strlen(response) > 0) {
            /* agregar \n como delimitador de mensaje */
            strncat(response, "\n", sizeof(response) - strlen(response) - 1);
            send(ctx->socket, response, strlen(response), MSG_NOSIGNAL);
            log_event(ip, port, buffer, response);
        }

        memset(buffer, 0, sizeof(buffer));
    }

    if (is_operator) unregister_operator(ctx->socket);
    close(ctx->socket);
    free(ctx);
    return NULL;
}

/* ─── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <puerto> <archivo_de_logs>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    g_log = fopen(argv[2], "a");
    if (!g_log) {
        perror("No se pudo abrir el archivo de logs");
        return 1;
    }

    /* Crear socket servidor */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    /* Reusar dirección para evitar "Address already in use" al reiniciar */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_addr.s_addr = INADDR_ANY;
    srv_addr.sin_port        = htons(port);

    if (bind(server_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, 20) < 0) {
        perror("listen"); return 1;
    }

    printf("=== Servidor IoT iniciado en puerto %d ===\n", port);
    printf("    Logs → %s\n\n", argv[2]);

    /* Bucle principal de aceptación */
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);

        int cli_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;   /* señal interrumpió accept, reintentar */
            perror("accept");
            continue;                        /* no finalizar el servidor por un error */
        }

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) { close(cli_fd); continue; }
        ctx->socket   = cli_fd;
        ctx->address  = cli_addr;          /* copia independiente para cada hilo */
        ctx->log_file = g_log;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ctx) != 0) {
            perror("pthread_create");
            free(ctx);
            close(cli_fd);
            continue;
        }
        pthread_detach(tid);
    }

    fclose(g_log);
    return 0;
}