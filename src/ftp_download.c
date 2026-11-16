#define _DEFAULT_SOURCE // diz ao glibc “ativa também as extensões BSD/GNU”
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

#define FTP_PORT 21     // porta de controlo
#define BUF_SIZE 8192


// copia os chars entre s e e (e não incluído) para um novo buffer.
// usei isto no parse_url para isolar os campos.
static char *dup_range(const char *s, const char *e) {
    size_t n = (size_t)(e - s);
    char *r = malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}


// separa o URL ftp://[user:pass@]host/path
// se não vier user:pass no URL, usa anonymous/anonymous
static int parse_url(const char *url, char **user, char **pass,
                     char **host, char **path) {
    const char *p, *slash, *at, *colon;

    *user = *pass = *host = *path = NULL;

    if (strncmp(url, "ftp://", 6) != 0)
        return -1;

    p = url + 6;
    slash = strchr(p, '/');
    at = strchr(p, '@');

    // se houver @ antes da primeira /, então há user:pass
    if (at && (!slash || at < slash)) {
        colon = memchr(p, ':', (size_t)(at - p));
        if (colon) {
            *user = dup_range(p, colon);
            *pass = dup_range(colon + 1, at);
        } else {
            // só veio user@, password vazia
            *user = dup_range(p, at);
            *pass = strdup("");
        }
        p = at + 1;
        slash = strchr(p, '/');
    }

    // sem user:pass no URL -> anónimo
    if (!*user) {
        *user = strdup("anonymous");
        *pass = strdup("anonymous");
    }

    if (!slash) {
        // não há path, usa a raiz
        *host = strdup(p);
        *path = strdup("/");
    } else {
        *host = dup_range(p, slash);
        *path = strdup(slash);
    }

    if (!*user || !*pass || !*host || !*path)
        return -1;

    return 0;
}


// o send pode escrever menos do que pedimos, por isso é preciso ciclo
static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}


// lê uma linha do socket e tira o \r no fim (FTP usa \r\n)
static int read_line(int fd, char *buf, size_t max) {
    size_t i = 0;
    char c;

    while (i + 1 < max) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[i++] = c;
    }
    buf[i] = '\0';
    return 0;
}


// junta s ao fim de out
static void append(char *out, size_t outsz, const char *s) {
    size_t len = strlen(out);
    if (len < outsz - 1)
        strncat(out, s, outsz - len - 1);
}


// lê uma resposta FTP completa e devolve o código (220, 331, 227, etc)
// uma resposta pode ter várias linhas: se o 4º char for '-' continua,
// se for ' ' é a última. está no RFC 959.
static int read_ftp_response(int fd, char *out, size_t outsz) {
    char line[1024];
    int code = 0;

    out[0] = '\0';

    while (1) {
        if (read_line(fd, line, sizeof(line)) < 0)
            return -1;

        if (out[0]) append(out, outsz, "\n");
        append(out, outsz, line);

        if (strlen(line) >= 4 &&
            isdigit((unsigned char)line[0]) &&
            isdigit((unsigned char)line[1]) &&
            isdigit((unsigned char)line[2]) &&
            (line[3] == ' ' || line[3] == '-')) {

            code = (line[0] - '0') * 100 +
                   (line[1] - '0') * 10 +
                   (line[2] - '0');

            if (line[3] == ' ')
                break;
        }
    }
    return code;
}


// envia um comando FTP e lê a resposta. imprime "C: ..." e "S: ..."
// para se ver o diálogo, tal como fizemos no telnet da 1ª aula
static int ftp_cmd(int fd, const char *cmd, char *resp, size_t respsz) {
    char buf[2048];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", cmd);

    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;

    printf("C: %s\n", cmd);

    if (write_all(fd, buf, strlen(buf)) < 0)
        return -1;

    int code = read_ftp_response(fd, resp, respsz);
    if (code > 0)
        printf("S: %s\n", resp);

    return code;
}


// abre a ligação de controlo (porta 21)
// gethostbyname -> getip.c , socket/connect -> clientTCP.c
static int open_control(const char *host) {
    struct hostent *h = gethostbyname(host);
    if (!h) {
        herror("gethostbyname");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(FTP_PORT);
    memcpy(&addr.sin_addr, h->h_addr, (size_t)h->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect control");
        close(fd);
        return -1;
    }

    return fd;
}


// abre a ligação de dados e vemos o modo passivo:
// o cliente é que se liga ao servidor, no IP/porto que vieram no 227
static int open_data(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket data");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "IP invalido: %s\n", ip);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect data");
        close(fd);
        return -1;
    }

    return fd;
}


// a resposta do PASV -> 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)
// o IP é h1.h2.h3.h4 e o porto é p1*256 + p2
static int parse_pasv(const char *resp, char *ip, size_t ipsz, int *port) {
    const char *p = strchr(resp, '(');
    int a, b, c, d, p1, p2;

    if (!p) return -1;
    if (sscanf(p, "(%d,%d,%d,%d,%d,%d)",
               &a, &b, &c, &d, &p1, &p2) != 6)
        return -1;

    snprintf(ip, ipsz, "%d.%d.%d.%d", a, b, c, d);
    *port = p1 * 256 + p2;
    return 0;
}


int main(int argc, char **argv) {
    char *user = NULL, *pass = NULL, *host = NULL, *path = NULL;
    int ctrl = -1, data = -1;
    FILE *f = NULL;
    int ret = 1;

    if (argc != 2) {
        fprintf(stderr, "Uso: %s ftp://[user:pass@]host/path\n", argv[0]);
        return 1;
    }

    // 1. parse do URL
    if (parse_url(argv[1], &user, &pass, &host, &path) < 0) {
        fprintf(stderr, "URL invalido. Usa ftp://[user:pass@]host/path\n");
        goto cleanup;
    }

    printf("URL   : %s\n", argv[1]);
    printf("Host  : %s\n", host);
    printf("Path  : %s\n", path);
    printf("User  : %s\n", user);

    char resp[4096];
    char cmd[2048];
    char ip[64];
    int port, code;

    // 2. ligação de controlo
    ctrl = open_control(host);
    if (ctrl < 0) {
        fprintf(stderr, "Falhou: ligacao de controlo\n");
        goto cleanup;
    }

    // o servidor manda logo 220 ao ligar
    code = read_ftp_response(ctrl, resp, sizeof(resp));
    printf("S: %s\n", resp);
    if (code != 220) {
        fprintf(stderr, "Falhou: greeting FTP\n");
        goto cleanup;
    }

    // 3. autenticação. USER pode dar 230 (já autenticado) ou 331 (pede pass)
    snprintf(cmd, sizeof(cmd), "USER %s", user);
    code = ftp_cmd(ctrl, cmd, resp, sizeof(resp));
    if (code != 230 && code != 331) {
        fprintf(stderr, "Falhou: USER\n");
        goto cleanup;
    }

    if (code == 331) {
        snprintf(cmd, sizeof(cmd), "PASS %s", pass);
        code = ftp_cmd(ctrl, cmd, resp, sizeof(resp));
        if (code != 230) {
            fprintf(stderr, "Falhou: PASS/autenticacao\n");
            goto cleanup;
        }
    }

    // 4. TYPE I = binário (não converte \r\n, é o que queremos para ficheiros)
    code = ftp_cmd(ctrl, "TYPE I", resp, sizeof(resp));
    if (code != 200) {
        fprintf(stderr, "Falhou: TYPE I\n");
        goto cleanup;
    }

    // 5. PASV: servidor responde 227 com IP/porto onde vai esperar a ligação de dados
    code = ftp_cmd(ctrl, "PASV", resp, sizeof(resp));
    if (code != 227) {
        fprintf(stderr, "Falhou: PASV\n");
        goto cleanup;
    }

    if (parse_pasv(resp, ip, sizeof(ip), &port) < 0) {
        fprintf(stderr, "Falhou: parse ao PASV\n");
        goto cleanup;
    }

    printf("Data  : %s:%d\n", ip, port);

    // 6. ligação de dados (somos nós que ligamos, é o modo passivo)
    data = open_data(ip, port);
    if (data < 0) {
        fprintf(stderr, "Falhou: ligacao de dados\n");
        goto cleanup;
    }

    // 7. RETR. 150 = vai começar a mandar, 125 = ligação já aberta
    snprintf(cmd, sizeof(cmd), "RETR %s", path);
    code = ftp_cmd(ctrl, cmd, resp, sizeof(resp));
    if (code != 150 && code != 125) {
        fprintf(stderr, "Falhou: RETR\n");
        goto cleanup;
    }

    // nome local = último componente do path, fica na CWD como o stor pediu
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (*base == '\0') base = "download";

    f = fopen(base, "wb");
    if (!f) {
        perror("fopen");
        goto cleanup;
    }

    // 8. receber o ficheiro pela ligação de dados e gravar
    char buf[BUF_SIZE];
    ssize_t n;
    long total = 0;
    int ioerr = 0;

    while ((n = recv(data, buf, sizeof(buf), 0)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) {
            perror("fwrite");
            ioerr = 1;
            break;
        }
        total += n;
    }

    if (n < 0) {
        perror("recv data");
        ioerr = 1;
    }

    fclose(f);
    f = NULL;
    close(data);
    data = -1;

    // o servidor confirma o fim da transferência com 226 (ou 250)
    code = read_ftp_response(ctrl, resp, sizeof(resp));
    printf("S: %s\n", resp);

    if (code != 226 && code != 250) {
        fprintf(stderr, "Falhou: fim da transferencia (code %d)\n", code);
        goto cleanup;
    }

    if (ioerr) {
        fprintf(stderr, "Aviso: houve erro local ao escrever o ficheiro\n");
        goto cleanup;
    }

    // 9. QUIT para fechar a ligação de controlo em condições
    ftp_cmd(ctrl, "QUIT", resp, sizeof(resp));

    printf("OK: ficheiro guardado como '%s' (%ld bytes)\n", base, total);
    ret = 0;

cleanup:
    if (f) fclose(f);
    if (data >= 0) close(data);
    if (ctrl >= 0) close(ctrl);
    free(user);
    free(pass);
    free(host);
    free(path);
    return ret;
}