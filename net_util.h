#ifndef NET_UTIL_H
#define NET_UTIL_H

#include "common.h"
#include <stddef.h>

// Capacità del buffer grezzo di input per ogni client
#define BUF_IN_SIZE 4096

// Buffer di INPUT: framing a righe terminate da '\n'
typedef struct {
    char   dati[BUF_IN_SIZE];
    size_t len;        /* byte attualmente nel buffer                          */
    int    scartando;  /* 1 = stiamo scartando una riga > MAX_LINE fino al '\n' */
} BufferIn;

void buf_in_init(BufferIn *b);

// Aggiunge n byte grezzi (appena letti) al buffer. Se è in corso lo scarto di
// una riga troppo lunga, consuma i byte fino al primo '\n'.
void buf_in_aggiungi(BufferIn *b, const char *src, size_t n);

// Estrae UNA riga completa (senza '\n' né eventuale '\r' finale) in out.
// Ritorna:
//    1  = riga estratta e copiata in out (terminata da '\0');
//    0  = nessuna riga completa: servono altri byte;
//   -1  = la riga supera MAX_LINE: viene scartata (il chiamante invii ERR).
// out deve avere dimensione >= MAX_LINE+1. */
int  buf_in_estrai_riga(BufferIn *b, char *out, size_t outsz);

// Coda di output dinamica per client
typedef struct {
    char  *dati;
    size_t len;   /* byte totali presenti                         */
    size_t off;   /* byte già inviati (primo non inviato = off)   */
    size_t cap;   /* capacità allocata                            */
} CodaOut;

void coda_init(CodaOut *c);
void coda_free(CodaOut *c);
int  coda_vuota(const CodaOut *c);                        /* 1 se non c'è nulla da inviare */
int  coda_accoda(CodaOut *c, const char *buf, size_t n);  /* 0 ok, -1 malloc fallita       */
int  coda_accoda_str(CodaOut *c, const char *s);          /* come sopra, con strlen        */

/* Invia quanto più possibile dalla coda sul socket fd (send con MSG_NOSIGNAL).
 * Ritorna:
 *    0 = ok, anche se l'invio è parziale o si è fermato su EAGAIN (riprovare
 *        al prossimo giro, quando il socket tornerà scrivibile);
 *   -1 = errore fatale sul socket: il chiamante deve rimuovere il client. */
int  coda_drena(CodaOut *c, int fd);

// Socket 
int imposta_nonblocking(int fd);            /* 0 ok, -1 errore */
int crea_listening_socket(int porta);       /* server: ritorna fd in ascolto o -1 */
int connetti_a(const char *host, int porta);/* client: ritorna fd connesso o -1   */

#endif /* NET_UTIL_H */
