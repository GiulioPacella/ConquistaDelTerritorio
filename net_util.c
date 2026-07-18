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
    // memchr cerca un byte dentro un blocco di memoria e ti dice dove si trova, se non lo trova ritorna NULL
    // cerca il primo '\n' dentro b->dati, fino a b->len byte
    char *nl = memchr(b->dati, '\n', b->len);
    // se non trova un '\n', allora restituisce NULL
    if (nl == NULL) {
        // se la lunghezza del buffer è maggiore o uguale a MAX_LINE, allora significa che la riga è troppo lunga e va scartata,
        // quindi mette in guardia il buf_in_aggiungi che gli arriverà il resto della riga troppo lunga (settando scartando = 1) e dovrà scartarlo fino al prossimo '\n'
        if (b->len >= MAX_LINE) {
            b->len = 0;
            b->scartando = 1;
            return -1;
        }
        return 0;
    }

    // calcola quanti byte di sono prima del /n (contenuto vero e proprio della riga, senza il terminatore)
    size_t linelen = (size_t)(nl - b->dati);   /* byte prima del '\n'        */
    // gli stessi byte più 1 per includere il terminatore '\n' (che poi verrà rimosso dal buffer)
    size_t consuma = linelen + 1;              /* includi il '\n'            */
    // è un caso limite per gestire il caso in cui la riga sia più lunga di MAX_LINE però ha un terminatore, quindi non può essere copiata in out
    int troppo_lunga = (linelen > MAX_LINE);

    if (!troppo_lunga) {
        size_t copylen = linelen;
        // tollera terminazione CRLF: rimuovi un eventuale '\r' finale, alcuni client lo fanno (Windows, telnet,...)
        if (copylen > 0 && b->dati[copylen - 1] == '\r') copylen--;
        // sicurezza in piu: se la riga è più lunga di outsz (ovvero il buffer del chiamante 
        // linea[MAX_LINE+1]), copia solo outsz-1 byte e termina con '\0' (per evitare buffer overflow)
        if (copylen >= outsz) copylen = outsz - 1;
        // copia i copylen byte della riga (senza /r e /n) dall'inizio del buffer in out
        memcpy(out, b->dati, copylen);
        // aggiunge il terminatore nullo, trasformando i byte grezzi in una stringa C valida per permettere al chiamante di usare funzioni come strcmp, printf, ecc
        out[copylen] = '\0';
    }

    // ora che la riga è stata consumata può essere rimossa dal buffer, spostando i byte rimanenti all'inizio del buffer e aggiornando la lunghezza
    memmove(b->dati, b->dati + consuma, b->len - consuma);
    b->len -= consuma;

    // anche qua il ritorno è lo stesso di prima: 1 se la riga è stata estratta correttamente, -1 se era troppo lunga e va scartata
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
    
    // crea la scoket usando come famiglia di indirizzi IPv4 (AF_INET), come tipo di socket TCP (SOCK_STREAM) e 
    //protocollo 0 (che indica al sistema operativo di scegliere il protocollo corretto in base al tipo di socket)
    // gestisce l'errore se la creazione della socket fallisce (ritorna -1)
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int yes = 1;
    
    // set sock option per riutilizzare l'indirizzo del socket (SO_REUSEADDR) in modo da poter riavviare il server senza dover aspettare che il sistema operativo liberi la porta
    // altrimenti il binding fallirebbe se il server viene riavviato subito dopo la chiusura (ci mette un minutino a liberare l'indirizzo TIME_WAIT)
    // yes = 1 significa attivare l'opzione, sizeof yes indica la dimensione del valore da impostare
    // SOL_SOCKET = l'opzione si applica al livello del socket stesso, non a un protocollo specifico
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
        close(fd);
        return -1;
    }

    // preparazione dell'indirizzo del socket per il binding: struct sockaddr_in è una struttura che rappresenta un indirizzo IPv4
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    // di nuovo IpV4 come famiglia di indrizzi, coerente col socket
    addr.sin_family = AF_INET;
    // htonl = host to network long, converte l'indirizzo IP da formato host (endianness della macchina) a formato di rete (big-endian)
    // INADDR_ANY = mettiti in ascolto su tutte le interfacce di rete disponibili (se mettessi localhost accetterebbe connessioni solo dalla stessa macchina)
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    // htons = host to network short, converte la porta da formato host a formato di rete (big-endian)
    addr.sin_port = htons((uint16_t)porta);

    // assegna l'indirizzo al socket appena creato, in modo che il socket sappia su quale indirizzo e porta ascoltare le connessioni in arrivo
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    // mette il socket in modalità ascolto, pronto ad accettare connessioni in arrivo (fino a 128 connessioni in coda)
    // rende il socket passivo, ovvero il suo unico scopo è accettare connessioni in arrivo, non inviare o ricevere dati
    // crea la lista di attesa per la connessione in arrivo, con una lunghezza massima di 128 connessioni in coda
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

// apre la connessione TCP dal client verso il server,
// prende host e porta resittuisce un file descriptor connesso o -1 in caso di errore
int connetti_a(const char *host, int porta) {
    // trasforma porta in stringa, perché getaddrinfo prende come argomento la porta come stringa
    char servizio[16];
    snprintf(servizio, sizeof servizio, "%d", porta);

    // vengono impostati tutti i parametri per la ricerca dell'indirizzo del server, in particolare:
    // hints.ai_family = AF_INET indica che vogliamo solo indirizzi IPv4
    // hints.ai_socktype = SOCK_STREAM indica che vogliamo solo socket TCP
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;       /* IPv4 */
    hints.ai_socktype = SOCK_STREAM;   /* TCP  */

    // getaddrinfo risolve il nome host e la porta in una lista di indirizzi, che vengono restituiti in res
    // se fallisce ritorna -1, altrimenti res punta a una lista di strutture addrinfo che contengono gli indirizzi del server
    
    if (getaddrinfo(host, servizio, &hints, &res) != 0) return -1;

    int fd = -1;
    // rp = puntatore alla lista di indirizzi restituiti da getaddrinfo, che viene scorsa per provare a connettersi a ciascun indirizzo fino a trovare quello corretto
    // il server può avere piu' indirizzi (IPv4, IPv6, hostname con piu' record A, ecc), quindi si prova a connettersi a ciascuno finché uno non funziona
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        // viene creata la socket per la comunicazione
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        // se la connessione riesce si esce dal ciclo, altrimenti si chiude la socket e si prova con il prossimo indirizzo
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;  /* connesso */
        close(fd);
        fd = -1;
    }
    // libera la memoria che getaddrinfo aveva occupato per creare la lista di indirizzi
    freeaddrinfo(res);
    return fd;
}
