#ifndef GAME_H
#define GAME_H

/*
 * game.h — stato e logica di gioco "Conquista del territorio".
 *
 * Sistema di coordinate (FISSATO):
 *   - origine (0,0) in alto a sinistra;
 *   - x = colonna, 0..MAPPA_W-1;  y = riga, 0..MAPPA_H-1;
 *   - U = y-1 (su), D = y+1 (giù), L = x-1 (sinistra), R = x+1 (destra).
 *
 * Identità giocatore: l'id coincide con l'indice di slot nell'array dei
 * giocatori del server. Le celle conquistate restano assegnate a quell'id anche
 * dopo la disconnessione (proprietà pubblica e persistente). LIMITE NOTO: se un
 * nuovo client riusa lo stesso slot, eredita quelle celle; è una semplificazione
 * didattica accettabile e in ogni caso lo stato viene azzerato a fine partita.
 */

#include "common.h"
#include "net_util.h"
#include <time.h>

/* ===== Un giocatore (slot del server) ===== */
typedef struct {
    int           attivo;          /* 1 = slot in uso (connessione presente)   */
    int           fd;              /* socket del client; -1 se slot libero     */
    StatoClient   stato;           /* CONNESSO / IN_GIOCO                       */
    int           id;              /* id giocatore (= indice di slot)          */
    char          nick[MAX_NICK + 1];
    int           x, y;            /* posizione corrente                       */
    int           punteggio;       /* numero di celle possedute (ricalcolato)  */
    unsigned char scoperto[MAPPA_H][MAPPA_W]; /* fog-of-war privato: 1 = muro visto */
    BufferIn      in;              /* buffer di input (framing a righe)        */
    CodaOut       out;             /* coda di output                           */
} Giocatore;

/* ===== Stato del mondo di gioco ===== */
typedef struct {
    unsigned char muri[MAPPA_H][MAPPA_W];      /* 1 = muro, 0 = libera          */
    int           proprieta[MAPPA_H][MAPPA_W]; /* id proprietario, -1 = nessuno */
    StatoPartita  stato;                       /* FERMA / ATTIVA                */
    time_t        inizio;                      /* istante di avvio partita      */
} Mappa;

/* ===== Ciclo di vita della partita ===== */
void gioco_genera_mappa(Mappa *m, unsigned int seed); /* muri casuali + proprietà = -1 */
void gioco_avvia(Mappa *m, unsigned int seed);        /* genera mappa, stato=ATTIVA, inizio=ora */
void gioco_termina(Mappa *m);                         /* stato=FERMA                    */
int  gioco_scaduta(const Mappa *m);                   /* 1 se trascorsi >= T_PARTITA s  */
int  gioco_secondi_residui(const Mappa *m);           /* secondi mancanti alla fine     */

/* ===== Posizionamento e movimento ===== */
/* Sceglie una cella libera casuale e la scrive in *x,*y. 0 = ok, -1 = mappa piena. */
int  gioco_spawn(const Mappa *m, int *x, int *y);

/* Rivela nel fog-of-war di g i muri della finestra (2*R_FOG+1) attorno alla sua posizione. */
void gioco_rivela_fog(const Mappa *m, Giocatore *g);

/* Esegue una mossa di g nella direzione dir ('U'/'D'/'L'/'R'). Ritorna:
 *    0 = mossa valida: g si è spostato e ha conquistato la cella;
 *    1 = destinazione è un muro: mossa rifiutata, muro rivelato a g;
 *    2 = destinazione fuori dai bordi: mossa rifiutata;
 *   -1 = direzione non riconosciuta. */
int  gioco_move(Mappa *m, Giocatore *g, char dir);

/* Ricalcola g->punteggio (per tutti gli slot attivi) scandendo la mappa. */
void gioco_aggiorna_punteggi(const Mappa *m, Giocatore *giocatori, int n_slot);

/* ===== Serializzazione dei messaggi (accodati nella CodaOut indicata) ===== */
void invia_local(CodaOut *out, const Mappa *m, const Giocatore *g);
void invia_global(CodaOut *out, const Mappa *m, const Giocatore *giocatori, int n_slot);
void invia_users(CodaOut *out, const Giocatore *giocatori, int n_slot);
void invia_gameover(CodaOut *out, const Mappa *m, const Giocatore *giocatori, int n_slot);

#endif /* GAME_H */
