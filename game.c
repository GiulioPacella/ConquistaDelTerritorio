#include "common.h"     /* PRIMO: feature-test macro */
#include "game.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ======================= Ciclo di vita ======================= */

void gioco_genera_mappa(Mappa *m, unsigned int seed) {
    srandom(seed);
    for (int y = 0; y < MAPPA_H; y++) {
        for (int x = 0; x < MAPPA_W; x++) {
            /* posizionamento pseudo-casuale dei muri secondo la densità scelta */
            m->muri[y][x] = (random() % 100 < DENSITA_MURI) ? 1 : 0;
            m->proprieta[y][x] = -1;   /* nessun proprietario */
        }
    }
}

void gioco_avvia(Mappa *m, unsigned int seed) {
    gioco_genera_mappa(m, seed);
    m->stato = PARTITA_ATTIVA;
    m->inizio = time(NULL);
}

void gioco_termina(Mappa *m) {
    m->stato = PARTITA_FERMA;
    m->fine  = time(NULL);   /* da qui parte la pausa di T_PAUSA secondi */
}

int gioco_pausa_residua(const Mappa *m) {
    if (m->stato == PARTITA_ATTIVA) return 0;
    if (m->fine == 0)               return 0;   /* nessuna partita conclusa finora */
    int trascorsi = (int)(time(NULL) - m->fine);
    int residui = T_PAUSA - trascorsi;
    return residui > 0 ? residui : 0;
}

int gioco_scaduta(const Mappa *m) {
    if (m->stato != PARTITA_ATTIVA) return 0;
    return (time(NULL) - m->inizio) >= T_PARTITA;
}

int gioco_secondi_residui(const Mappa *m) {
    if (m->stato != PARTITA_ATTIVA) return T_PARTITA;
    int trascorsi = (int)(time(NULL) - m->inizio);
    int residui = T_PARTITA - trascorsi;
    return residui > 0 ? residui : 0;
}

/* ======================= Posizionamento e movimento ======================= */

int gioco_spawn(const Mappa *m, int *x, int *y) {
    /* conta le celle libere */
    int libere = 0;
    for (int yy = 0; yy < MAPPA_H; yy++)
        for (int xx = 0; xx < MAPPA_W; xx++)
            if (!m->muri[yy][xx]) libere++;
    if (libere == 0) return -1;   /* mappa piena di muri */

    /* sceglie la k-esima cella libera in ordine di scansione */
    int k = (int)(random() % libere);
    for (int yy = 0; yy < MAPPA_H; yy++) {
        for (int xx = 0; xx < MAPPA_W; xx++) {
            if (!m->muri[yy][xx]) {
                if (k == 0) { *x = xx; *y = yy; return 0; }
                k--;
            }
        }
    }
    return -1;   /* non dovrebbe accadere */
}

void gioco_rivela_fog(const Mappa *m, Giocatore *g) {
    for (int dy = -R_FOG; dy <= R_FOG; dy++) {
        for (int dx = -R_FOG; dx <= R_FOG; dx++) {
            int x = g->x + dx;
            int y = g->y + dy;
            if (x < 0 || x >= MAPPA_W || y < 0 || y >= MAPPA_H) continue;
            if (m->muri[y][x]) g->scoperto[y][x] = 1;
        }
    }
}

int gioco_move(Mappa *m, Giocatore *g, char dir) {
    int nx = g->x;
    int ny = g->y;
    switch (dir) {
        case 'U': ny = g->y - 1; break;
        case 'D': ny = g->y + 1; break;
        case 'L': nx = g->x - 1; break;
        case 'R': nx = g->x + 1; break;
        default:  return -1;
    }

    /* fuori dai bordi → rifiutata */
    if (nx < 0 || nx >= MAPPA_W || ny < 0 || ny >= MAPPA_H) return 2;

    /* muro → rifiutata, ma il muro viene rivelato a questo giocatore */
    if (m->muri[ny][nx]) {
        g->scoperto[ny][nx] = 1;
        return 1;
    }

    /* cella libera → spostamento + conquista/ribaltamento */
    g->x = nx;
    g->y = ny;
    m->proprieta[ny][nx] = g->id;
    gioco_rivela_fog(m, g);
    return 0;
}

void gioco_aggiorna_punteggi(const Mappa *m, Giocatore *giocatori, int n_slot) {
    for (int i = 0; i < n_slot; i++)
        if (giocatori[i].attivo) giocatori[i].punteggio = 0;

    for (int y = 0; y < MAPPA_H; y++) {
        for (int x = 0; x < MAPPA_W; x++) {
            int id = m->proprieta[y][x];
            if (id >= 0 && id < n_slot && giocatori[id].attivo)
                giocatori[id].punteggio++;
        }
    }
}

/* ======================= Serializzazione ======================= */

void invia_local(CodaOut *out, const Mappa *m, const Giocatore *g) {
    char riga[64];

    snprintf(riga, sizeof riga, "%s %d %d %d\n", REP_LOCAL, g->x, g->y, R_FOG);
    coda_accoda_str(out, riga);

    /* finestra (2*R_FOG+1) x (2*R_FOG+1) centrata sulla posizione di g */
    char linea[2 * R_FOG + 2];          /* caratteri della finestra + '\n' */
    for (int dy = -R_FOG; dy <= R_FOG; dy++) {
        int col = 0;
        for (int dx = -R_FOG; dx <= R_FOG; dx++) {
            int x = g->x + dx;
            int y = g->y + dy;
            char c;
            if (x < 0 || x >= MAPPA_W || y < 0 || y >= MAPPA_H) {
                c = SIM_IGNOTO;                       /* fuori mappa */
            } else if (dx == 0 && dy == 0) {
                c = SIM_IO;                           /* la mia posizione */
            } else if (m->muri[y][x] && g->scoperto[y][x]) {
                c = SIM_MURO;                         /* muro che ho scoperto */
            } else if (m->proprieta[y][x] >= 0) {
                c = simbolo_proprietario(m->proprieta[y][x]);
            } else {
                c = SIM_LIBERA;
            }
            linea[col++] = c;
        }
        linea[col++] = '\n';
        coda_accoda(out, linea, (size_t)col);
    }

    coda_accoda_str(out, REP_END "\n");
}

void invia_global(CodaOut *out, const Mappa *m, const Giocatore *giocatori, int n_slot) {
    /* conta i giocatori loggati */
    int nplayers = 0;
    for (int i = 0; i < n_slot; i++)
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO) nplayers++;

    char riga[MAX_NICK + 64];
    snprintf(riga, sizeof riga, "%s %d %d %d\n", REP_GLOBAL, MAPPA_W, MAPPA_H, nplayers);
    coda_accoda_str(out, riga);

    /* mappa delle proprietà: H righe da W caratteri (senza '@', muri non mostrati) */
    char linea[MAPPA_W + 2];
    for (int y = 0; y < MAPPA_H; y++) {
        for (int x = 0; x < MAPPA_W; x++) {
            int id = m->proprieta[y][x];
            linea[x] = (id >= 0) ? simbolo_proprietario(id) : SIM_LIBERA;
        }
        linea[MAPPA_W] = '\n';
        coda_accoda(out, linea, MAPPA_W + 1);
    }

    /* elenco giocatori con posizione e punteggio */
    for (int i = 0; i < n_slot; i++) {
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO) {
            snprintf(riga, sizeof riga, "P %d %s %d %d %d\n",
                     giocatori[i].id, giocatori[i].nick,
                     giocatori[i].x, giocatori[i].y, giocatori[i].punteggio);
            coda_accoda_str(out, riga);
        }
    }

    coda_accoda_str(out, REP_END "\n");
}

void invia_users(CodaOut *out, const Giocatore *giocatori, int n_slot) {
    int n = 0;
    for (int i = 0; i < n_slot; i++)
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO) n++;

    char riga[MAX_NICK + 32];
    snprintf(riga, sizeof riga, "%s %d\n", REP_USERS, n);
    coda_accoda_str(out, riga);

    for (int i = 0; i < n_slot; i++) {
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO) {
            snprintf(riga, sizeof riga, "%d %s\n", giocatori[i].id, giocatori[i].nick);
            coda_accoda_str(out, riga);
        }
    }

    coda_accoda_str(out, REP_END "\n");
}

void invia_gameover(CodaOut *out, const Mappa *m, const Giocatore *giocatori, int n_slot) {
    (void)m;
    /* raccoglie i giocatori loggati in un array di indici e li ordina per
       punteggio decrescente (selection sort: pochi elementi, semplice). */
    int idx[MAX_CLIENT];
    int n = 0;
    for (int i = 0; i < n_slot; i++)
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO)
            idx[n++] = i;

    for (int a = 0; a < n; a++) {
        int max = a;
        for (int b = a + 1; b < n; b++)
            if (giocatori[idx[b]].punteggio > giocatori[idx[max]].punteggio) max = b;
        int t = idx[a]; idx[a] = idx[max]; idx[max] = t;
    }

    char riga[MAX_NICK + 32];
    snprintf(riga, sizeof riga, "%s %d\n", REP_GAMEOVER, n);
    coda_accoda_str(out, riga);

    for (int pos = 0; pos < n; pos++) {
        snprintf(riga, sizeof riga, "%d %s %d\n",
                 pos + 1, giocatori[idx[pos]].nick, giocatori[idx[pos]].punteggio);
        coda_accoda_str(out, riga);
    }

    coda_accoda_str(out, REP_END "\n");
}
