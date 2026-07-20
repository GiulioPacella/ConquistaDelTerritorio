#ifndef COMMON_H
#define COMMON_H

/*
 * common.h — parametri globali, limiti e tipi condivisi tra server e client.
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stddef.h>   /* size_t */

// parametri di gioco
#define MAPPA_H        20    /* numero di righe   (coordinata y: 0..MAPPA_H-1) */
#define MAPPA_W        20    /* numero di colonne (coordinata x: 0..MAPPA_W-1) */
#define R_FOG          2     /* raggio fog-of-war: finestra (2*R_FOG+1) = 5x5   */
#define T_BROADCAST    20     /* secondi tra un broadcast GLOBAL e il successivo */
#define T_FLASH        2    /* durata della "luce globale" dopo ogni GLOBAL (s); < T_BROADCAST */
#define T_PARTITA      100  /* durata massima di una partita, in secondi       */
#define T_PAUSA        10   /* pausa dopo il GAMEOVER: per questi secondi la classifica                              
                             resta leggibile e MOVE/LOGIN non avviano una nuova partita */
#define DENSITA_MURI   20    /* densità muri in percentuale (~20%)              */

// protezioni contro buffer overflow
#define MAX_LINE       1024  /* lunghezza massima di una riga di protocollo (byte) */
#define MAX_NICK       31    /* lunghezza massima di un nickname (caratteri)       */
#define MAX_PASS       31    /* lunghezza massima di una password (caratteri)      */
#define MAX_CLIENT     1000  /* numero massimo di client simultanei                */

// macchina a stati del client: connessione, login, partita
typedef enum {
    ST_CONNESSO = 0,  /* connesso ma non loggato: ammessi REGISTER, LOGIN, QUIT   */
    ST_IN_GIOCO       /* loggato / in gioco: ammessi MOVE, WHO, QUIT              */
} StatoClient;

// stato della partita: ferma (nessuna partita in corso) o attiva (partita in corso)
typedef enum {
    PARTITA_FERMA = 0, /* nessuna partita in corso: parte al primo login          */
    PARTITA_ATTIVA     /* partita in corso: il timer globale sta scorrendo         */
} StatoPartita;

#endif /* COMMON_H */
