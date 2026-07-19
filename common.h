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
#define T_BROADCAST    20     /* secondi tra un broadcast GLOBAL e il successivo */
#define T_FLASH        2    /* durata della "luce globale" dopo ogni GLOBAL (s); < T_BROADCAST */
#define T_PARTITA      100  /* durata massima di una partita, in secondi       */
#define T_PAUSA        10   /* pausa dopo il GAMEOVER: per questi secondi la classifica
                               resta leggibile e MOVE/LOGIN non avviano una nuova partita */
#define DENSITA_MURI   20    /* densità muri in percentuale (~20%)              */

/* ===== Limiti e protezione contro input malevolo ===== */
#define MAX_LINE       1024  /* lunghezza massima di una riga di protocollo (byte) */
#define MAX_NICK       31    /* lunghezza massima di un nickname (caratteri)       */
#define MAX_PASS       31    /* lunghezza massima di una password (caratteri)      */
#define MAX_CLIENT     1000  /* numero massimo di client simultanei                */

/*
 * Nota sul limite MAX_CLIENT e FD_SETSIZE.
 *
 * La traccia chiede "nessun limite a priori" al numero di utenti. Un numero
 * davvero illimitato non è ottenibile con select(): tutti i file descriptor
 * gestiti devono restare sotto FD_SETSIZE (tipicamente 1024) perché FD_SET su un
 * fd >= FD_SETSIZE è undefined behavior. Il limite quindi NON è una scelta
 * arbitraria ma è imposto dal meccanismo di multiplexing adottato.
 *
 * MAX_CLIENT è perciò portato vicino a quel tetto (1000), lasciando margine per
 * gli fd riservati (stdin/stdout/stderr, il listening socket, l'handle
 * temporaneo su users.dat durante register/login). Il server applica comunque
 * una guardia a runtime: se un fd accettato risulta >= FD_SETSIZE la connessione
 * viene rifiutata (vedi accetta_connessioni in server.c).
 */

/* ===== Macchina a stati del client (vista dal server) ===== */
typedef enum {
    ST_CONNESSO = 0,  /* connesso ma non loggato: ammessi REGISTER, LOGIN, QUIT   */
    ST_IN_GIOCO       /* loggato / in gioco: ammessi MOVE, WHO, QUIT              */
} StatoClient;

/* ===== Stato globale della partita ===== */
typedef enum {
    PARTITA_FERMA = 0, /* nessuna partita in corso: parte al primo login          */
    PARTITA_ATTIVA     /* partita in corso: il timer globale sta scorrendo         */
} StatoPartita;

#endif /* COMMON_H */
