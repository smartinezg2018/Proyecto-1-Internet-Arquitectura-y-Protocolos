/*
 * server.c — Servidor Central IoT
 * Uso: ./server <puerto> <archivo_de_logs>
 *
 * Protocolo de aplicación (texto plano, delimitado por \n):
 *   REGISTER|<sensor_id>|<tipo>          → OK|REGISTERED  o  ERR|...
 *   DATA|<sensor_id>|<tipo>|<valor>      → OK|ACK
 *   GET_STATE                            → id:tipo:valor:HH:MM:SS;...  o  EMPTY
 *   GET_ALERTS                           → alerta\nalerta\n...         o  NONE
 *   LOGIN|<usuario>|<clave>              → OK|<rol>  o  ERR|UNAUTHORIZED
 *   SUBSCRIBE                            → OK|SUBSCRIBED  + mensajes ALERT|... push
 *   GET / HTTP/1.1                       → dashboard HTML con Basic Auth
 */

#define _POSIX_C_SOURCE 200112L   /* garantiza struct addrinfo en todos los sistemas */

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
#include <arpa/inet.h>    /* inet_ntop, htons, INADDR_ANY */
#include <sys/time.h>     /* struct timeval para SO_RCVTIMEO */

/* ─── Constantes ─────────────────────────────────────────────── */
#define MAX_SENSORS   50
#define MAX_OPERATORS 20
#define MAX_ALERTS    100
#define BUF_SIZE      4096

/* Umbrales para generar alertas automáticas */
#define TEMP_MAX  80.0    /* °C   */
#define VIBR_MAX  10.0    /* mm/s */
#define ENER_MAX  18.0    /* kWh  */
#define HUM_MAX   90.0    /* %    */

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
    printf("[%s] %s:%d | RX: %.80s | TX: %.80s\n", ts, ip, port, rx, tx);
    if (g_log) {
        fprintf(g_log, "[%s] %s:%d | RX: %s | TX: %s\n", ts, ip, port, rx, tx);
        fflush(g_log);
    }
    pthread_mutex_unlock(&log_lock);
}

/* ─── Timestamp actual (HH:MM:SS) ────────────────────────────── */
void current_ts(char *buf, size_t sz) {
    time_t now = time(NULL);
    strftime(buf, sz, "%H:%M:%S", localtime(&now));
}

/* ─── Alertas: guardar y notificar operadores suscritos ──────── */
void push_alert(const char *msg) {
    /* Guardar en historial circular */
    pthread_mutex_lock(&alerts_lock);
    if (alert_count < MAX_ALERTS) {
        strncpy(alerts[alert_count], msg, 255);
        alert_count++;
    }
    pthread_mutex_unlock(&alerts_lock);

    /* Enviar a todos los operadores con conexión SUBSCRIBE activa */
    char notif[300];
    snprintf(notif, sizeof(notif), "ALERT|%s\n", msg);

    pthread_mutex_lock(&operators_lock);
    for (int i = 0; i < operator_count; i++) {
        if (operators[i].socket > 0) {
            if (send(operators[i].socket, notif, strlen(notif), MSG_NOSIGNAL) < 0)
                operators[i].socket = 0;   /* operador desconectado */
        }
    }
    pthread_mutex_unlock(&operators_lock);
}

/* ─── Verificar umbrales y generar alertas ────────────────────── */
void check_thresholds(const char *sensor_id, const char *tipo, const char *valor) {
    char msg[256];
    int alerta = 0;

    /* STAT es texto, no número: alerta si reporta falla */
    if (strcmp(tipo, "STAT") == 0) {
        if (strstr(valor, "Falla") || strstr(valor, "falla")) {
            snprintf(msg, sizeof(msg), "FALLA en %s: estado=%s", sensor_id, valor);
            alerta = 1;
        }
    } else {
        /* Para tipos numéricos, comparar contra umbral */
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

/* ─── Unidad de medida por tipo de sensor ────────────────────── */
const char *unidad_de_tipo(const char *tipo) {
    if (strcmp(tipo, "TEMP") == 0) return "C";
    if (strcmp(tipo, "VIBR") == 0) return "mm/s";
    if (strcmp(tipo, "ENER") == 0) return "kWh";
    if (strcmp(tipo, "HUM")  == 0) return "%";
    return "";
}

/* ─── REGISTER ────────────────────────────────────────────────── */
void handle_register(const char *msg, char *resp) {
    /* Formato: REGISTER|<sensor_id>|<tipo> */
    char tmp[256];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    strtok(tmp, "|");                    /* descarta "REGISTER" */
    char *id   = strtok(NULL, "|");
    char *tipo = strtok(NULL, "|\r\n");

    if (!id || !tipo) { strcpy(resp, "ERR|FORMATO_INVALIDO"); return; }

    pthread_mutex_lock(&sensors_lock);
    /* Si el sensor ya existe, marcarlo como activo (reconexión) */
    for (int i = 0; i < sensor_count; i++) {
        if (strcmp(sensors[i].id, id) == 0) {
            sensors[i].activo = 1;
            pthread_mutex_unlock(&sensors_lock);
            strcpy(resp, "OK|REGISTERED");
            return;
        }
    }
    /* Sensor nuevo */
    if (sensor_count < MAX_SENSORS) {
        strncpy(sensors[sensor_count].id,     id,   31);
        strncpy(sensors[sensor_count].tipo,   tipo, 31);
        strncpy(sensors[sensor_count].unidad, unidad_de_tipo(tipo), 15);
        strcpy (sensors[sensor_count].valor,  "---");
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
    /* Formato: DATA|<sensor_id>|<tipo>|<valor> */
    char tmp[256];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
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
    /* Auto-registro si el sensor envía DATA sin haber hecho REGISTER */
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
     * Reenvía LOGIN|usuario|clave al AuthServer externo y devuelve su respuesta.
     * El host del AuthServer se lee de la variable de entorno AUTH_HOST.
     *   - Desarrollo local:   AUTH_HOST=localhost ./server 8080 logs.txt
     *   - Docker-compose:     AUTH_HOST=auth-service (nombre del contenedor)
     *
     * IMPORTANTE: tmp y msg_nl son 256/260 bytes, NO BUF_SIZE (4096).
     * LOGIN|usuario|clave tiene máximo ~133 chars. Usar BUF_SIZE aquí
     * causaba stack smashing al acumularse con los frames de handle_http
     * y handle_client (que ya tienen buffer[4096]+response[4096]).
     */
    char tmp[256];
    strncpy(tmp, msg, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    const char *auth_host_env = getenv("AUTH_HOST");
    const char *auth_host = auth_host_env ? auth_host_env : "auth-service";
    const char *auth_port = "9000";

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(auth_host, auth_port, &hints, &res) != 0) {
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

    /* Timeout de 5s para no bloquear el hilo si el AuthServer no responde */
    struct timeval tv = {5, 0};
    setsockopt(auth_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char msg_nl[260];
    snprintf(msg_nl, sizeof(msg_nl), "%s\n", tmp);
    send(auth_fd, msg_nl, strlen(msg_nl), 0);

    /* Leer respuesta caracter a caracter hasta \n o timeout */
    char auth_resp[256] = {0};
    int  ar_len = 0;
    char ch;
    while (ar_len < (int)sizeof(auth_resp) - 1) {
        int r = recv(auth_fd, &ch, 1, 0);
        if (r <= 0) break;
        if (ch == '\n') break;
        if (ch != '\r') auth_resp[ar_len++] = ch;
    }
    close(auth_fd);

    if (ar_len == 0) { strcpy(resp, "ERR|AUTH_TIMEOUT"); return; }
    strncpy(resp, auth_resp, 255);  /* resp es siempre 256 bytes — nunca BUF_SIZE */
}

/* ─── HTTP con formulario de login propio ────────────────────────
 *
 * Reemplaza HTTP Basic Auth (que mostraba popup nativo del browser)
 * por un formulario HTML con la misma estética que el dashboard Java.
 *
 * Sesiones: array estático global (sin malloc, sin heap).
 *   - MAX_WEB_SESSIONS slots de tamaño fijo
 *   - Cada sesión: token[32] + usuario[64] + rol[32] + timestamp
 *   - TTL: 10 minutos desde la última actividad
 *   - Token generado con rand() sobre chars hexadecimales
 *
 * Rutas:
 *   GET  /        → dashboard (requiere cookie de sesión válida)
 *   GET  /login   → formulario de login
 *   POST /login   → procesa credenciales, crea sesión, setea cookie
 *   GET  /logout  → invalida sesión, redirige a /login
 *
 * Buffers grandes (table_rows, alerts_html, body) en HEAP con
 * calloc/free — evita stack smashing al acumularse con los frames
 * de handle_client (buffer[4096]+response[4096]) y handle_login.
 * ─────────────────────────────────────────────────────────────── */

#define MAX_WEB_SESSIONS 32
#define WEB_TOKEN_LEN    32
#define WEB_SESSION_TTL  600   /* segundos — 10 minutos */

typedef struct {
    char   token[WEB_TOKEN_LEN + 1];
    char   usuario[64];
    char   rol[32];
    time_t ts;       /* timestamp de creación/último uso */
    int    activa;
} web_session_t;

static web_session_t web_sessions[MAX_WEB_SESSIONS];
static pthread_mutex_t web_sessions_lock = PTHREAD_MUTEX_INITIALIZER;

/* Generar token hexadecimal aleatorio de WEB_TOKEN_LEN chars */
static void web_gen_token(char *out) {
    static int seeded = 0;
    if (!seeded) { srand((unsigned)time(NULL) ^ (unsigned)getpid()); seeded = 1; }
    for (int i = 0; i < WEB_TOKEN_LEN; i++)
        out[i] = "0123456789abcdef"[rand() % 16];
    out[WEB_TOKEN_LEN] = '\0';
}

/* Crear sesión — devuelve índice o -1 si no hay espacio */
static int web_session_create(const char *usuario, const char *rol) {
    pthread_mutex_lock(&web_sessions_lock);
    time_t now = time(NULL);
    /* Limpiar sesiones expiradas */
    for (int i = 0; i < MAX_WEB_SESSIONS; i++)
        if (web_sessions[i].activa && now - web_sessions[i].ts > WEB_SESSION_TTL)
            web_sessions[i].activa = 0;
    /* Buscar slot libre */
    for (int i = 0; i < MAX_WEB_SESSIONS; i++) {
        if (!web_sessions[i].activa) {
            web_gen_token(web_sessions[i].token);
            strncpy(web_sessions[i].usuario, usuario, 63);
            strncpy(web_sessions[i].rol,     rol,     31);
            web_sessions[i].ts     = now;
            web_sessions[i].activa = 1;
            pthread_mutex_unlock(&web_sessions_lock);
            return i;
        }
    }
    pthread_mutex_unlock(&web_sessions_lock);
    return -1;
}

/* Buscar sesión por token — devuelve puntero o NULL si no existe/expiró */
static web_session_t *web_session_find(const char *token) {
    if (!token || strlen(token) != WEB_TOKEN_LEN) return NULL;
    time_t now = time(NULL);
    pthread_mutex_lock(&web_sessions_lock);
    for (int i = 0; i < MAX_WEB_SESSIONS; i++) {
        if (web_sessions[i].activa &&
            strncmp(web_sessions[i].token, token, WEB_TOKEN_LEN) == 0 &&
            now - web_sessions[i].ts <= WEB_SESSION_TTL) {
            web_sessions[i].ts = now;   /* renovar TTL en cada uso */
            pthread_mutex_unlock(&web_sessions_lock);
            return &web_sessions[i];
        }
    }
    pthread_mutex_unlock(&web_sessions_lock);
    return NULL;
}

/* Invalidar sesión por token */
static void web_session_destroy(const char *token) {
    pthread_mutex_lock(&web_sessions_lock);
    for (int i = 0; i < MAX_WEB_SESSIONS; i++)
        if (web_sessions[i].activa &&
            strncmp(web_sessions[i].token, token, WEB_TOKEN_LEN) == 0) {
            web_sessions[i].activa = 0;
            break;
        }
    pthread_mutex_unlock(&web_sessions_lock);
}

/* Extraer valor de una cookie del header Cookie: */
static void web_get_cookie(const char *request, const char *name,
                            char *out, size_t out_sz) {
    out[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "%s=", name);
    const char *p = strstr(request, needle);
    if (!p) return;
    p += strlen(needle);
    size_t i = 0;
    while (*p && *p != ';' && *p != '\r' && *p != '\n' && i < out_sz - 1)
        out[i++] = *p++;
    out[i] = '\0';
}

/* ── Respuestas HTTP auxiliares ────────────────────────────────── */

static void http_redirect(int sock, const char *location) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n", location);
    send(sock, buf, strlen(buf), 0);
}

static void http_redirect_with_cookie(int sock, const char *location,
                                       const char *token) {
    char buf[512];
    snprintf(buf, sizeof(buf),
        "HTTP/1.1 302 Found\r\n"
        "Location: %s\r\n"
        "Set-Cookie: sid=%s; HttpOnly; Path=/; Max-Age=%d\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        location, token, WEB_SESSION_TTL);
    send(sock, buf, strlen(buf), 0);
}

/* ── Página de login ────────────────────────────────────────────── */
static void http_send_login(int sock, const char *error) {
    /* Body fijo en stack — es pequeño y no varía */
    char body[2048];
    int blen = snprintf(body, sizeof(body),
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<title>IoT Monitor - Login</title>"
        "<style>"
        "body{font-family:sans-serif;background:#1e1e2e;color:#cdd6f4;"
        "display:flex;align-items:center;justify-content:center;"
        "min-height:100vh;margin:0}"
        ".card{background:#313244;padding:2em 2.5em;border-radius:12px;width:300px}"
        "h2{margin:0 0 .3em;color:#cdd6f4;font-size:1.2em}"
        "p.sub{color:#6c7086;margin:0 0 1.5em;font-size:.9em}"
        "label{display:block;margin-bottom:.3em;font-size:.9em;color:#a6adc8}"
        "input{width:100%%;box-sizing:border-box;padding:.5em .7em;"
        "margin-bottom:1em;background:#1e1e2e;border:1px solid #45475a;"
        "border-radius:6px;color:#cdd6f4;font-size:1em}"
        "button{width:100%%;padding:.6em;background:#89b4fa;color:#1e1e2e;"
        "border:none;border-radius:6px;font-size:1em;font-weight:bold;cursor:pointer}"
        "button:hover{background:#74c7ec}"
        ".err{color:#f38ba8;font-size:.85em;margin-bottom:.8em;min-height:1.2em}"
        "</style></head><body>"
        "<div class=\"card\">"
        "<h2>IoT Monitor</h2>"
        "<p class=\"sub\">EAFIT - Iniciar sesion</p>"
        "<form method=\"POST\" action=\"/login\">"
        "<label>Usuario</label>"
        "<input type=\"text\" name=\"u\" autofocus required>"
        "<label>Contrasena</label>"
        "<input type=\"password\" name=\"p\" required>"
        "<p class=\"err\">%s</p>"
        "<button type=\"submit\">Ingresar</button>"
        "</form></div></body></html>",
        error ? error : "");

    char hdr[256];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n", blen);
    send(sock, hdr, strlen(hdr), 0);
    send(sock, body, blen, 0);
}

/* ── Dashboard principal ────────────────────────────────────────── */
static void http_send_dashboard(int sock, const char *usuario,
                                  const char *rol, const char *token) {
    /* Buffers grandes en heap — evita stack smashing */
    char *rows  = calloc(8192,  1);
    char *ahml  = calloc(4096,  1);
    char *body  = calloc(20480, 1);
    if (!rows || !ahml || !body) { free(rows); free(ahml); free(body); return; }

    /* Tabla de sensores */
    pthread_mutex_lock(&sensors_lock);
    int nsens = sensor_count;
    for (int j = 0; j < nsens; j++) {
        const char *bg = strstr(sensors[j].valor, "Falla")
            ? " style=\"background:#45231d;color:#f38ba8\"" : "";
        char row[384];
        snprintf(row, sizeof(row),
            "<tr%s><td>%s</td><td>%s</td><td>%s %s</td><td>%s</td></tr>\n",
            bg, sensors[j].id, sensors[j].tipo,
            sensors[j].valor, sensors[j].unidad, sensors[j].timestamp);
        if (strlen(rows) + strlen(row) < 8191) strcat(rows, row);
    }
    pthread_mutex_unlock(&sensors_lock);

    /* Alertas recientes (últimas 8, orden descendente) */
    pthread_mutex_lock(&alerts_lock);
    int nalerts = alert_count;
    int astart  = (alert_count > 8) ? alert_count - 8 : 0;
    for (int j = alert_count - 1; j >= astart; j--) {
        char li[300];
        snprintf(li, sizeof(li),
            "<li style=\"color:#f38ba8\">%s</li>\n", alerts[j]);
        if (strlen(ahml) + strlen(li) < 4095) strcat(ahml, li);
    }
    pthread_mutex_unlock(&alerts_lock);

    char awrap[4120];
    if (ahml[0]) snprintf(awrap, sizeof(awrap), "<ul>%s</ul>", ahml);
    else strcpy(awrap, "<p style=\"color:#6c7086\">Sin alertas registradas</p>");

    int blen = snprintf(body, 20480,
        "<!DOCTYPE html><html><head>"
        "<meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"5\">"
        "<title>IoT Monitor</title>"
        "<style>"
        "body{font-family:sans-serif;margin:0;background:#1e1e2e;color:#cdd6f4}"
        ".bar{background:#313244;padding:.8em 1.5em;display:flex;"
        "align-items:center;justify-content:space-between;"
        "border-bottom:1px solid #45475a}"
        ".bar h1{margin:0;font-size:1.1em;color:#cdd6f4}"
        ".info{display:flex;gap:.8em;align-items:center;font-size:.9em}"
        ".b{background:#1e1e2e;padding:3px 10px;border-radius:4px;font-size:.85em}"
        ".blu{color:#89b4fa}.gry{color:#6c7086}"
        "a.logout{color:#f38ba8;text-decoration:none;font-size:.85em;"
        "border:1px solid #f38ba8;padding:3px 10px;border-radius:4px}"
        "a.logout:hover{background:#f38ba8;color:#1e1e2e}"
        ".main{padding:1.5em}"
        "table{border-collapse:collapse;width:100%%}"
        "th,td{border:1px solid #45475a;padding:9px 12px;text-align:left}"
        "th{background:#313244}"
        "h2{margin:1.2em 0 .5em;font-size:.9em;text-transform:uppercase;"
        "letter-spacing:.05em;color:#6c7086}"
        "ul{list-style:none;padding:.5em 1em;margin:0;"
        "background:#313244;border-radius:6px}"
        "li{padding:4px 0;border-bottom:1px solid #45475a;font-size:.9em}"
        "li:last-child{border:none}"
        "</style></head><body>"
        "<div class=\"bar\"><h1>Sistema de Monitoreo IoT</h1>"
        "<div class=\"info\">"
        "<span class=\"b blu\">%s</span>"
        "<span class=\"b gry\">%s</span>"
        "<a class=\"logout\" href=\"/logout\">Cerrar sesion</a>"
        "</div></div>"
        "<div class=\"main\">"
        "<h2>Sensores activos (%d)</h2>"
        "<table><tr><th>ID</th><th>Tipo</th><th>Valor</th><th>Ultima lectura</th></tr>"
        "%s</table>"
        "<h2>Alertas recientes (%d total)</h2>%s"
        "<p style=\"color:#6c7086;font-size:.8em;margin-top:1em\">"
        "Recarga automatica cada 5 segundos</p>"
        "</div></body></html>",
        usuario, rol, nsens,
        rows[0] ? rows : "<tr><td colspan=\"4\" style=\"color:#6c7086\">"
                         "Sin sensores conectados</td></tr>",
        nalerts, awrap);

    /* Header con cookie para mantener la sesión */
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Set-Cookie: sid=%s; HttpOnly; Path=/; Max-Age=%d\r\n"
        "Connection: close\r\n\r\n",
        blen, token, WEB_SESSION_TTL);

    send(sock, hdr, strlen(hdr), 0);
    send(sock, body, blen, 0);
    free(rows); free(ahml); free(body);
}

/* ── Dispatcher HTTP ────────────────────────────────────────────── */
void handle_http(int sock, const char *request, const char *ip, int port) {
    char method[8] = {0}, path[64] = {0};
    sscanf(request, "%7s %63s", method, path);

    /* GET /logout — invalidar sesión y redirigir a /login */
    if (strcmp(path, "/logout") == 0) {
        char token[WEB_TOKEN_LEN + 1] = {0};
        web_get_cookie(request, "sid", token, sizeof(token));
        web_session_destroy(token);
        http_redirect(sock, "/login");
        log_event(ip, port, "GET /logout", "302 sesion cerrada");
        return;
    }

    /* GET /login — mostrar formulario */
    if (strcmp(path, "/login") == 0 && strcmp(method, "GET") == 0) {
        http_send_login(sock, NULL);
        log_event(ip, port, "GET /login", "200 OK");
        return;
    }

    /* POST /login — procesar credenciales */
    if (strcmp(path, "/login") == 0 && strcmp(method, "POST") == 0) {
        const char *body_start = strstr(request, "\r\n\r\n");
        if (!body_start) { http_send_login(sock, "Peticion invalida"); return; }
        body_start += 4;

        /* Parsear u=usuario&p=clave (URL-encoded simple, sin espacios) */
        char usuario[64] = {0}, clave[64] = {0};
        const char *pu = strstr(body_start, "u=");
        if (pu) { pu += 2; int k=0; while(*pu && *pu!='&' && k<63) usuario[k++]=*pu++; }
        const char *pp = strstr(body_start, "p=");
        if (pp) { pp += 2; int k=0; while(*pp && *pp!='&' && k<63) clave[k++]=*pp++; }

        if (!usuario[0] || !clave[0]) {
            http_send_login(sock, "Completa todos los campos");
            return;
        }

        /* Validar con AuthServer */
        char lmsg[300], lresp[256];
        snprintf(lmsg, sizeof(lmsg), "LOGIN|%s|%s", usuario, clave);
        handle_login(lmsg, lresp);

        if (strncmp(lresp, "OK", 2) != 0) {
            http_send_login(sock, "Usuario o contrasena incorrectos");
            log_event(ip, port, "POST /login", "401 credenciales invalidas");
            return;
        }

        char rol[32] = "operador";
        char *pipe = strchr(lresp, '|');
        if (pipe) strncpy(rol, pipe + 1, sizeof(rol) - 1);

        int idx = web_session_create(usuario, rol);
        if (idx < 0) { http_send_login(sock, "Servidor ocupado, reintenta"); return; }

        /* Redirigir al dashboard con cookie de sesión */
        http_redirect_with_cookie(sock, "/", web_sessions[idx].token);
        log_event(ip, port, "POST /login", "302 sesion creada");
        return;
    }

    /* GET / — dashboard (requiere sesión válida) */
    char token[WEB_TOKEN_LEN + 1] = {0};
    web_get_cookie(request, "sid", token, sizeof(token));
    web_session_t *sess = web_session_find(token);

    if (!sess) {
        http_redirect(sock, "/login");
        log_event(ip, port, "GET /", "302 sin sesion");
        return;
    }

    http_send_dashboard(sock, sess->usuario, sess->rol, sess->token);
    log_event(ip, port, "GET /", "200 OK");
}

/* ─── Registro y baja de operadores suscritos ─────────────────── */
void register_operator(int sock, const char *ip, int port) {
    pthread_mutex_lock(&operators_lock);
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

        /* ── Detectar tipo de mensaje y despachar ── */
        if (strncmp(buffer, "GET /",  5) == 0 ||
            strncmp(buffer, "POST /", 6) == 0 ||
            strncmp(buffer, "GET H",  5) == 0) {
            /* Petición HTTP del browser */
            handle_http(ctx->socket, buffer, ip, port);
            break;   /* HTTP cierra la conexión tras la respuesta */
        }
        else if (strncmp(buffer, "REGISTER", 8) == 0)
            handle_register(buffer, response);
        else if (strncmp(buffer, "DATA", 4) == 0)
            handle_data(buffer, response);
        else if (strncmp(buffer, "GET_STATE", 9) == 0)
            handle_get_state(response, sizeof(response));
        else if (strncmp(buffer, "GET_ALERTS", 10) == 0)
            handle_get_alerts(response, sizeof(response));
        else if (strncmp(buffer, "LOGIN", 5) == 0)
            handle_login(buffer, response);
        else if (strncmp(buffer, "SUBSCRIBE", 9) == 0) {
            register_operator(ctx->socket, ip, port);
            is_operator = 1;
            strcpy(response, "OK|SUBSCRIBED");
        }
        else
            strcpy(response, "ERR|COMANDO_DESCONOCIDO");

        /* Enviar respuesta con \n como delimitador de mensaje */
        if (strlen(response) > 0) {
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

    g_log = fopen(argv[2], "a");
    if (!g_log) { perror("No se pudo abrir el archivo de logs"); return 1; }

    /* Crear socket servidor TCP */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    /* SO_REUSEADDR evita "Address already in use" al reiniciar rápido */
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv_addr;
    memset(&srv_addr, 0, sizeof(srv_addr));
    srv_addr.sin_family      = AF_INET;
    srv_addr.sin_addr.s_addr = INADDR_ANY;
    srv_addr.sin_port        = htons(atoi(argv[1]));

    if (bind(server_fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) < 0)
        { perror("bind"); return 1; }
    if (listen(server_fd, 20) < 0)
        { perror("listen"); return 1; }

    printf("=== Servidor IoT iniciado en puerto %s ===\n", argv[1]);
    printf("    Logs -> %s\n\n", argv[2]);

    /* Bucle principal: aceptar clientes y lanzar hilo por cada uno */
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);

        int cli_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) {
            if (errno == EINTR) continue;   /* señal interrumpió accept, reintentar */
            perror("accept");
            continue;                        /* no terminar el servidor por un error */
        }

        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        if (!ctx) { close(cli_fd); continue; }
        ctx->socket   = cli_fd;
        ctx->address  = cli_addr;   /* copia independiente para cada hilo */
        ctx->log_file = g_log;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ctx) != 0) {
            perror("pthread_create");
            free(ctx);
            close(cli_fd);
            continue;
        }
        pthread_detach(tid);   /* el hilo se limpia solo al terminar */
    }

    fclose(g_log);
    return 0;
}