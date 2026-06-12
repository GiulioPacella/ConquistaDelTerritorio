#include "common.h"     /* PRIMO: definisce le feature-test macro */
#include "net_util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

/* ======================= Buffer di INPUT ======================= */

void buf_in_init(BufferIn *b) {
    b->len = 0;
    b->scartando = 0;
}

void buf_in_aggiungi(BufferIn *b, const char *src, size_t n) {
    /* Se stiamo scartando una riga troppo lunga, butta via tutto fino al primo
       '\n' (incluso) e poi riprendi a bufferizzare il resto. */
    if (b->scartando) {
        const char *nl = memchr(src, '\n', n);
        if (nl == NULL) return;             /* ancora dentro la riga lunga */
        b->scartando = 0;
        size_t saltati = (size_t)(nl - src) + 1;
        src += saltati;
        n   -= saltati;
    }
    size_t spazio = BUF_IN_SIZE - b->len;
    if (n > spazio) n = spazio;             /* difesa: non sforare il buffer */
    memcpy(b->dati + b->len, src, n);
    b->len += n;
}

int buf_in_estrai_riga(BufferIn *b, char *out, size_t outsz) {
    char *nl = memchr(b->dati, '\n', b->len);
    if (nl == NULL) {
        /* Nessuna riga completa. Se ciò che è accumulato supera già MAX_LINE,
           la riga in corso è troppo lunga: scarta e segnala. */
        if (b->len >= MAX_LINE) {
            b->len = 0;
            b->scartando = 1;
            return -1;
        }
        return 0;
    }

    size_t linelen = (size_t)(nl - b->dati);   /* byte prima del '\n'        */
    size_t consuma = linelen + 1;              /* includi il '\n'            */
    int troppo_lunga = (linelen > MAX_LINE);

    if (!troppo_lunga) {
        size_t copylen = linelen;
        /* tollera terminazione CRLF: rimuovi un eventuale '\r' finale */
        if (copylen > 0 && b->dati[copylen - 1] == '\r') copylen--;
        if (copylen >= outsz) copylen = outsz - 1; /* sicurezza */
        memcpy(out, b->dati, copylen);
        out[copylen] = '\0';
    }

    /* compatta il buffer rimuovendo la riga consumata */
    memmove(b->dati, b->dati + consuma, b->len - consuma);
    b->len -= consuma;

    return troppo_lunga ? -1 : 1;
}

/* ======================= Coda di OUTPUT ======================= */

void coda_init(CodaOut *c) {
    c->dati = NULL;
    c->len = 0;
    c->off = 0;
    c->cap = 0;
}

void coda_free(CodaOut *c) {
    free(c->dati);
    coda_init(c);
}

int coda_vuota(const CodaOut *c) {
    return c->off >= c->len;
}

int coda_accoda(CodaOut *c, const char *buf, size_t n) {
    /* Se tutto ciò che c'era è stato inviato, riparti da zero. */
    if (c->off == c->len) {
        c->off = 0;
        c->len = 0;
    }
    /* Compatta se l'offset è cresciuto molto, per non far gonfiare la coda. */
    if (c->off > 0 && c->off * 2 > c->len) {
        memmove(c->dati, c->dati + c->off, c->len - c->off);
        c->len -= c->off;
        c->off = 0;
    }
    if (c->len + n > c->cap) {
        size_t nuova = (c->cap != 0) ? c->cap : 256;
        while (c->len + n > nuova) nuova *= 2;
        char *p = realloc(c->dati, nuova);
        if (p == NULL) return -1;
        c->dati = p;
        c->cap = nuova;
    }
    memcpy(c->dati + c->len, buf, n);
    c->len += n;
    return 0;
}

int coda_accoda_str(CodaOut *c, const char *s) {
    return coda_accoda(c, s, strlen(s));
}

int coda_drena(CodaOut *c, int fd) {
    while (c->off < c->len) {
        ssize_t s = send(fd, c->dati + c->off, c->len - c->off, MSG_NOSIGNAL);
        if (s > 0) {
            c->off += (size_t)s;
            continue;
        }
        if (s < 0 && errno == EINTR) continue;                 /* riprova subito */
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;                  /* socket pieno: riprova al prossimo giro */
        return -1;                     /* errore fatale (EPIPE/ECONNRESET/...)   */
    }
    /* coda interamente svuotata: azzera gli indici */
    c->off = 0;
    c->len = 0;
    return 0;
}

/* ======================= Socket ======================= */

int imposta_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

int crea_listening_socket(int porta) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        close(fd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)porta);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    if (imposta_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int connetti_a(const char *host, int porta) {
    char servizio[16];
    snprintf(servizio, sizeof servizio, "%d", porta);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;       /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;   /* TCP  */

    if (getaddrinfo(host, servizio, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;  /* connesso */
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}
