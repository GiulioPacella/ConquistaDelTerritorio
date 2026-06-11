#ifndef COMMON_H
#define COMMON_H

/*
 * common.h — parametri globali, limiti e tipi condivisi tra server e client.
 *
 * IMPORTANTE: questo header definisce le "feature-test macro" e DEVE essere il
 * PRIMO #include di ogni file .c del progetto, prima di qualunque header di
 * sistema. Così funzioni come fileno(), random()/srandom(), snprintf(),
 * fcntl(), getaddrinfo() e la costante MSG_NOSIGNAL risultano dichiarate
 * correttamente e non generano warning con -Wall -Wextra -pedantic -std=c11.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>   /* size_t */

/* ===== Parametri di gioco (valori di default, configurabili qui) ===== */
#define MAPPA_H        20    /* numero di righe   (coordinata y: 0..MAPPA_H-1) */
#define MAPPA_W        20    /* numero di colonne (coordinata x: 0..MAPPA_W-1) */
#define R_FOG          2     /* raggio fog-of-war: finestra (2*R_FOG+1) = 5x5   */
#define T_BROADCAST    5     /* secondi tra un broadcast GLOBAL e il successivo */
#define T_FLASH        3     /* durata della "luce globale" dopo ogni GLOBAL (s); < T_BROADCAST */
#define T_PARTITA      180   /* durata massima di una partita, in secondi       */
#define DENSITA_MURI   20    /* densità muri in percentuale (~20%)              */

/* ===== Limiti e protezione contro input malevolo ===== */
#define MAX_LINE       1024  /* lunghezza massima di una riga di protocollo (byte) */
#define MAX_NICK       31    /* lunghezza massima di un nickname (caratteri)       */
#define MAX_PASS       31    /* lunghezza massima di una password (caratteri)      */
#define MAX_CLIENT     64    /* numero massimo di client simultanei                */

/*
 * Nota sul limite FD_SETSIZE: il server usa select(), quindi tutti i file
 * descriptor devono restare sotto FD_SETSIZE (tipicamente 1024). MAX_CLIENT è
 * tenuto basso (64) ed è ampiamente entro il limite; non è richiesto scalare
 * oltre questa soglia.
 */

/* ===== Macchina a stati del client (vista dal server) ===== */
typedef enum {
    ST_CONNESSO = 0,  /* connesso ma non loggato: ammessi REGISTER, LOGIN, QUIT   */
    ST_IN_GIOCO       /* loggato / in gioco: ammessi MOVE, WHO, MAP, QUIT         */
} StatoClient;

/* ===== Stato globale della partita ===== */
typedef enum {
    PARTITA_FERMA = 0, /* nessuna partita in corso: parte al primo login          */
    PARTITA_ATTIVA     /* partita in corso: il timer globale sta scorrendo         */
} StatoPartita;

#endif /* COMMON_H */
