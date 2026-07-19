#include "common.h"     /* PRIMO: feature-test macro */
#include "protocol.h"
#include "net_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>

/*
 * client.c — client interattivo non bloccante.
 *
 * Usa select() su due sorgenti: la tastiera (stdin) e il socket verso il
 * server. Così l'utente può digitare comandi mentre i broadcast arrivano, senza
 * mai bloccarsi. I dati dal server sono bufferizzati e processati una riga per
 * volta (framing a '\n', come il server). Il client è "stupido": inoltra i
 * comandi e disegna ciò che riceve; tutta la logica è sul server.
 *
 * Comandi: si possono digitare direttamente (REGISTER/LOGIN/MOVE/WHO/QUIT)
 * oppure usare le scorciatoie di movimento W/A/S/D (su/sinistra/giù/destra).
 */

/* ===== Rendering: colori ANSI opzionali ===== */
static int usa_colori = 0;

/*
 * Modello persistente della mappa conosciuta dal client.
 *
 * Esiste UNA sola mappa (la griglia 20x20): la "torcia" (messaggi LOCAL) e lo
 * "scatto" globale periodico (messaggi GLOBAL) si fondono entrambi su questo
 * modello. Regola di visibilita': SOLO i muri scoperti sono ricordati e
 * disegnati permanentemente (fog-of-war asimmetrica); proprieta' delle celle e
 * posizioni degli altri giocatori si disegnano UNICAMENTE dentro la torcia
 * (2*R_FOG+1)x(2*R_FOG+1) centrata sul giocatore.
 */
static char          cli_prop[MAPPA_H][MAPPA_W];   /* proprietario per cella, SIM_LIBERA = neutra */
static unsigned char cli_muro[MAPPA_H][MAPPA_W];   /* 1 = muro scoperto (PERSISTENTE)             */
static unsigned char cli_altri[MAPPA_H][MAPPA_W];  /* 1 = altro giocatore presente (da ultimo GLOBAL) */
static int  cli_self_id = -1;                      /* mio id (estratto da "OK ... id=...")        */
static int  cli_self_x = -1, cli_self_y = -1;      /* mia posizione corrente                       */
static int  cli_pronto = 0;                        /* 1 quando c'e' una mappa da disegnare         */
static time_t cli_flash_fine = 0;                  /* istante di spegnimento della "luce globale"; 0 = spenta */

// azzera la memoria del client della mappa, in modo che non ci siano dati residui da una partita precedente
static void reset_modello(void) {
    for (int y = 0; y < MAPPA_H; y++)
        for (int x = 0; x < MAPPA_W; x++)
            cli_prop[y][x] = SIM_LIBERA;
    memset(cli_muro, 0, sizeof cli_muro);
    memset(cli_altri, 0, sizeof cli_altri);
    cli_self_x = cli_self_y = -1;
    cli_pronto = 0;
    cli_flash_fine = 0;
}

/* Colore di un proprietario (palette ciclica per distinguerli). */
static const char *colore_proprietario(char c) {
    static const char *pal[] = {
        "\033[1;91m", "\033[1;92m", "\033[1;94m",
        "\033[1;95m", "\033[1;96m", "\033[1;31m"
    };
    int idx = 0;
    if (c >= '0' && c <= '9')      idx = c - '0';
    else if (c >= 'a' && c <= 'z') idx = 10 + (c - 'a');
    return pal[idx % 6];
}

/* Emette un carattere di cella (col = stringa colore ANSI, ignorata senza tty). */
static void emetti(char c, const char *col) {
    if (usa_colori && col != NULL) printf("%s%c\033[0m", col, c);
    else putchar(c);
}

#define COL_CORNICE "\033[1;33m"   /* cornice della torcia: giallo, come la @ propria */

/*
 * Disegna l'intera mappa conosciuta. Prima cancella la mappa precedente
 * (schermo + scrollback con \033[3J: sul terminale resta sempre UNA sola
 * mappa). Visibilita': fuori dalla torcia si disegnano SOLO i muri gia'
 * scoperti, tranne quando la "luce globale" e' accesa (per T_FLASH secondi
 * dopo ogni GLOBAL): allora proprieta' e giocatori si vedono ovunque, ma i
 * muri non scoperti restano comunque nascosti.
 *
 * Cornice della torcia: la corona di celle a distanza R_FOG+1 dal giocatore
 * viene disegnata con i caratteri '+', '-' e '|' AL POSTO del suo contenuto
 * (nessuna riga extra: la griglia resta MAPPA_H righe esatte); ai bordi della
 * mappa la corona e' tagliata. La griglia e' fatta di colonne da 2 caratteri
 * (slot separatore + slot cella): sui lati orizzontali della corona anche i
 * separatori diventano '-', cosi' la linea risulta continua.
 */
static void disegna_mappa(void) {
    if (!cli_pronto) return;
    int luce = (cli_flash_fine != 0 && time(NULL) < cli_flash_fine);
    int pos_nota = (cli_self_x >= 0);
    int rc = R_FOG + 1;   /* raggio della corona-cornice attorno alla torcia */

    if (usa_colori) fputs("\033[H\033[2J\033[3J", stdout); /* home + pulisci schermo e scrollback */
    else            putchar('\n');
    if (luce) printf("=== Mappa %dx%d  (tu = @) -- LUCE GLOBALE ACCESA ===\n", MAPPA_W, MAPPA_H);
    else      printf("=== Mappa %dx%d  (tu = @) ===\n", MAPPA_W, MAPPA_H);

    for (int y = 0; y < MAPPA_H; y++) {
        fputs("  ", stdout);
        for (int x = 0; x <= MAPPA_W; x++) {
            /* slot separatore: '-' fra due celle dei lati orizzontali della corona */
            if (pos_nota && abs(y - cli_self_y) == rc &&
                x >= 1 && x > cli_self_x - rc && x <= cli_self_x + rc)
                emetti('-', COL_CORNICE);
            else
                putchar(' ');
            if (x >= MAPPA_W) break;   /* separatore finale: nessuna cella dopo */

            /* slot cella */
            int adx = abs(x - cli_self_x), ady = abs(y - cli_self_y);
            int dentro = pos_nota && adx <= R_FOG && ady <= R_FOG;
            int corona = pos_nota && adx <= rc && ady <= rc && (adx == rc || ady == rc);
            if (corona)
                emetti((adx == rc && ady == rc) ? '+' : (ady == rc ? '-' : '|'),
                       COL_CORNICE);                                /* cornice al posto della cella */
            else if (dentro && x == cli_self_x && y == cli_self_y)
                emetti(SIM_IO, "\033[1;93m");                       /* io: giallo brillante */
            else if ((dentro || luce) && cli_altri[y][x])
                emetti(SIM_IO, "\033[1;96m");                       /* altro giocatore visibile: ciano */
            else if (cli_muro[y][x])
                emetti(SIM_MURO, "\033[90m");                       /* muro ricordato (sempre visibile) */
            else if ((dentro || luce) && cli_prop[y][x] != SIM_LIBERA)
                emetti(cli_prop[y][x], colore_proprietario(cli_prop[y][x]));
            else
                emetti(SIM_LIBERA, "\033[37m");                     /* neutra / non visibile */
        }
        putchar('\n');
    }
    fflush(stdout);
}

static void stampa_legenda(void) {
    printf("  Legenda:  @ giocatori (il tuo in colore diverso)   %c muro scoperto (resta visibile)   %c libera/non visibile   0-9/a-z proprietari\n",
           SIM_MURO, SIM_LIBERA);
    printf("  Visibilita': proprieta' e giocatori solo dentro la torcia %dx%d (delimitata dalla cornice); fuori restano solo i muri scoperti.\n",
           2 * R_FOG + 1, 2 * R_FOG + 1);
    printf("  Ogni %d s si accende la LUCE GLOBALE per %d s: si vede tutta la mappa (tranne i muri non ancora scoperti).\n",
           T_BROADCAST, T_FLASH);
}

static void stampa_aiuto(void) {
    printf("=== Conquista del territorio — client ===\n");
    printf("Comandi: REGISTER <nick> <pass> | LOGIN <nick> <pass> | WHO | QUIT\n");
    printf("Movimento: MOVE <U|D|L|R>  oppure le scorciatoie  W A S D\n");
    stampa_legenda();
    printf("=========================================\n");
    fflush(stdout);
}

/* ===== Stato del parser dei messaggi multi-riga del server ===== */
#define MAX_RIGHE 128
#define MAX_COLS  (MAX_LINE + 1)

static int  in_blocco = 0;
static char header[MAX_COLS];
static char corpo[MAX_RIGHE][MAX_COLS];
static int  n_corpo = 0;

/*
 * LOCAL = la "torcia": finestra (2r+1) centrata su (cx,cy). Aggiorna il modello
 * con cio' che la torcia illumina (muri ricordati, proprieta', mia posizione).
 */
static void aggiorna_da_local(void) {
    int cx, cy, r;
    if (sscanf(header, "%*s %d %d %d", &cx, &cy, &r) != 3) return;
    cli_self_x = cx;
    cli_self_y = cy;
    for (int i = 0; i < n_corpo; i++) {
        int dy = i - r;
        for (int j = 0; corpo[i][j] != '\0'; j++) {
            int dx = j - r;
            int ax = cx + dx, ay = cy + dy;
            if (ax < 0 || ax >= MAPPA_W || ay < 0 || ay >= MAPPA_H) continue;
            char c = corpo[i][j];
            if (c == SIM_IGNOTO)        continue;                     /* fuori mappa */
            else if (c == SIM_IO)       cli_prop[ay][ax] = simbolo_proprietario(cli_self_id);
            else if (c == SIM_MURO)     cli_muro[ay][ax] = 1;         /* memoria permanente */
            else if (c == SIM_LIBERA)   cli_prop[ay][ax] = SIM_LIBERA;
            else                        cli_prop[ay][ax] = c;         /* proprietario */
        }
    }
    cli_pronto = 1;
    disegna_mappa();
}

/*
 * GLOBAL = lo "scatto" periodico: snapshot autorevole delle proprieta' di tutte
 * le celle + posizione di tutti i giocatori. NON rivela i muri (quindi non tocca
 * cli_muro): le zone inesplorate appaiono come celle neutre.
 */
static void aggiorna_da_global(void) {
    int w, h, n;
    if (sscanf(header, "%*s %d %d %d", &w, &h, &n) != 3) return;
    if (w > MAPPA_W) w = MAPPA_W;
    if (h > MAPPA_H) h = MAPPA_H;
    for (int y = 0; y < h && y < n_corpo; y++)
        for (int x = 0; x < w && corpo[y][x] != '\0'; x++)
            cli_prop[y][x] = corpo[y][x];
    /* posizioni dei giocatori (righe "P id nick x y score") */
    memset(cli_altri, 0, sizeof cli_altri);
    for (int i = h; i < n_corpo; i++) {
        int id, x, y, score;
        char nick[MAX_NICK + 1];
        if (sscanf(corpo[i], "P %d %31s %d %d %d", &id, nick, &x, &y, &score) == 5) {
            if (x < 0 || x >= MAPPA_W || y < 0 || y >= MAPPA_H) continue;
            if (id == cli_self_id) { cli_self_x = x; cli_self_y = y; }
            else                   cli_altri[y][x] = 1;
        }
    }
    cli_pronto = 1;
    cli_flash_fine = time(NULL) + T_FLASH;   /* accende la "luce globale" per T_FLASH secondi */
    disegna_mappa();
}

static void rendi_users(void) {
    printf("\n--- Giocatori loggati ---\n");
    for (int i = 0; i < n_corpo; i++) {
        int id;
        char nick[MAX_NICK + 1];
        if (sscanf(corpo[i], "%d %31s", &id, nick) == 2)
            printf("    [%d] %s\n", id, nick);
    }
    fflush(stdout);
}

static void rendi_gameover(void) {
    printf("\n############ FINE PARTITA — Classifica ############\n");
    for (int i = 0; i < n_corpo; i++) {
        int pos, score;
        char nick[MAX_NICK + 1];
        if (sscanf(corpo[i], "%d %31s %d", &pos, nick, &score) == 3)
            printf("    %d) %-16s %d celle\n", pos, nick, score);
    }
    printf("###################################################\n");
    printf("(resti connesso: fra %d secondi il prossimo movimento avvia una nuova partita)\n",
           T_PAUSA);
    fflush(stdout);
}

static void rendi_blocco(void) {
    if (strncmp(header, REP_LOCAL, strlen(REP_LOCAL)) == 0)         aggiorna_da_local();
    else if (strncmp(header, REP_GLOBAL, strlen(REP_GLOBAL)) == 0)  aggiorna_da_global();
    else if (strncmp(header, REP_USERS, strlen(REP_USERS)) == 0)    rendi_users();
    else if (strncmp(header, REP_GAMEOVER, strlen(REP_GAMEOVER)) == 0) {
        rendi_gameover();
        reset_modello();   /* nuova partita = mappa nuova: scorda muri e proprieta' */
    }
}

/* Processa una riga ricevuta dal server. */
static void processa_server(const char *linea) {
    // se in_blocco è 1 significa che il client sta leggendo un blocco di messaggi multi-riga (LOCAL, GLOBAL, USERS, GAMEOVER)
    if (in_blocco) 
    {
        // se la riga è REP_END significa che il blocco di messaggi multi-riga è terminato, quindi chiama rendi_blocco() per processare
        // il blocco e azzera in_blocco e n_corpo
        if (strcmp(linea, REP_END) == 0) {
            rendi_blocco();
            in_blocco = 0;
            n_corpo = 0;
        } 
        // altrimenti se n_corpo < MAX_RIGHE significa che il blocco non è terminato e c'è ancora spazio nel buffer corpo,
        // quindi copia la riga in corpo[n_corpo] e incrementa n_corpo
        else if (n_corpo < MAX_RIGHE) {
            snprintf(corpo[n_corpo], MAX_COLS, "%s", linea);
            n_corpo++;
        }
        return;
    }

    /* riga di intestazione */
    // se la riga inizia con REP_OK o REP_ERR significa che è una risposta a un comando del client, quindi cerca "id=" per estrarre l'id del giocatore
    if (strncmp(linea, REP_OK, strlen(REP_OK)) == 0 ||
        strncmp(linea, REP_ERR, strlen(REP_ERR)) == 0) {
        const char *p = strstr(linea, "id=");   /* "OK login id=<n>" */
        if (p != NULL) sscanf(p, "id=%d", &cli_self_id);
        printf("[server] %s\n", linea);
        fflush(stdout);
        return;
    }
    if 
    // casi in cui incontra un blocco di messaggi multi-riga (LOCAL, GLOBAL, USERS, GAMEOVER)
    (
        // compara strlen(REP_LOCAL) caratteri di linea con REP_LOCAL, se sono uguali ritorna 0
        strncmp(linea, REP_LOCAL, strlen(REP_LOCAL)) == 0 ||
        strncmp(linea, REP_GLOBAL, strlen(REP_GLOBAL)) == 0 ||
        strncmp(linea, REP_USERS, strlen(REP_USERS)) == 0 ||
        strncmp(linea, REP_GAMEOVER, strlen(REP_GAMEOVER)) == 0) {
        // scrive linea in header 
        snprintf(header, sizeof header, "%s", linea);
        in_blocco = 1;
        n_corpo = 0;
        return;
    }
    /* riga non riconosciuta: mostrala comunque */
    printf("[server] %s\n", linea);
    fflush(stdout);
}

/* ===== Invio comandi al server ===== */
static int invia(int fd, const char *s) {
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t w = send(fd, s + off, len - off, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static void gestisci_input(int fd, const char *linea) {
    if (linea[0] == '\0') return;   /* riga vuota: ignora */

    char cmd[MAX_LINE + 16];
    /* scorciatoie di movimento: singolo carattere W/A/S/D */
    if (linea[1] == '\0') {
        char c = (char)tolower((unsigned char)linea[0]);
        const char *mv = NULL;
        if (c == 'w')      mv = CMD_MOVE " U";
        else if (c == 's') mv = CMD_MOVE " D";
        else if (c == 'a') mv = CMD_MOVE " L";
        else if (c == 'd') mv = CMD_MOVE " R";
        if (mv != NULL) {
            snprintf(cmd, sizeof cmd, "%s\n", mv);
            invia(fd, cmd);
            return;
        }
    }
    snprintf(cmd, sizeof cmd, "%s\n", linea);
    invia(fd, cmd);
}

int main(int argc, char *argv[]) {
    // controlla la validità degli argomenti
    if (argc != 3) {
        fprintf(stderr, "uso: %s <host> <porta>\n", argv[0]);
        return 1;
    }
    int porta = atoi(argv[2]);
    if (porta < 1 || porta > 65535) {
        fprintf(stderr, "porta non valida: %s\n", argv[2]);
        return 1;
    }

    int fd = connetti_a(argv[1], porta);
    if (fd < 0) {
        fprintf(stderr, "connessione fallita a %s:%d\n", argv[1], porta);
        return 1;
    }

    // gestione dei segnali: ignora SIGPIPE (altrimenti il client termina se il server chiude la connessione)
    signal(SIGPIPE, SIG_IGN);
    // abilita i colori se l'output è un terminale
    usa_colori = isatty(STDOUT_FILENO);

    // azzera il modello della mappa, in modo che non ci siano dati residui da una partita precedente
    reset_modello();

    stampa_aiuto();

    // crea i buffer per la lettura dei dati dal server e dalla tastiera, e li inizializza
    BufferIn rete, tast;
    buf_in_init(&rete);
    buf_in_init(&tast);

    int attivo = 1;
    while (attivo) {
        // inizializziamo il set di file descriptor da monitorare per la lettura, 
        // il client deve ascoltare la tastiera (quello che scrivi tu) e il server (le risposte che ti manda)
        fd_set rfds;
        FD_ZERO(&rfds);
        // aggiunge la tastiera al set di file descriptor da monitorare per la lettura
        FD_SET(STDIN_FILENO, &rfds);
        // aggiunge il server al set di file descriptor da monitorare per la lettura
        FD_SET(fd, &rfds);
        int maxfd = (fd > STDIN_FILENO) ? fd : STDIN_FILENO;

        /* se la "luce globale" e' accesa, select ha un timeout pari al tempo
           residuo, cosi' allo scadere la mappa si ridisegna in vista-torcia */
        struct timeval tv;
        struct timeval *ptv = NULL;
        if (cli_flash_fine != 0) {
            time_t ora = time(NULL);
            if (ora >= cli_flash_fine) {
                cli_flash_fine = 0;       /* luce scaduta: spegni e ridisegna */
                disegna_mappa();
            } else {
                tv.tv_sec = cli_flash_fine - ora;
                tv.tv_usec = 0;
                ptv = &tv;
            }
        }

        int p = select(maxfd + 1, &rfds, NULL, NULL, ptv);
        if (p < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p == 0) continue;   /* timeout: il prossimo giro spegne la luce */

        /* --- dati dal server --- */
        // se è rimasto nel set vuol dire che ci sono dei dati da leggere dal server, quindi li leggiamo e li processiamo
        if (FD_ISSET(fd, &rfds)) {
            char tmp[2048];
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n > 0) {
                // aggiungi i dati letti dal server al buffer di rete, e poi estrai le righe complete dal buffer e processale
                // questa parte è uguale a quella che c'è in server.c
                buf_in_aggiungi(&rete, tmp, (size_t)n);
                char l[MAX_LINE + 1];
                int r;
                while ((r = buf_in_estrai_riga(&rete, l, sizeof l)) != 0) {
                    if (r == -1) continue;   /* riga troppo lunga: ignora */
                    processa_server(l);
                }
            } else if (n == 0) {
                printf("\n[connessione chiusa dal server]\n");
                attivo = 0;
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                printf("\n[errore di lettura dalla rete]\n");
                attivo = 0;
            }
        }

        /* --- input da tastiera --- */
        if (attivo && FD_ISSET(STDIN_FILENO, &rfds)) {
            char tmp[1024];
            ssize_t n = read(STDIN_FILENO, tmp, sizeof tmp);
            if (n > 0) {
                buf_in_aggiungi(&tast, tmp, (size_t)n);
                char l[MAX_LINE + 1];
                int r;
                while ((r = buf_in_estrai_riga(&tast, l, sizeof l)) != 0) {
                    if (r == -1) { printf("[client] riga troppo lunga, ignorata\n"); continue; }
                    gestisci_input(fd, l);
                }
            } else if (n == 0) {
                invia(fd, CMD_QUIT "\n");   /* EOF da tastiera: esci con grazia */
                attivo = 0;
            }
        }
    }

    close(fd);
    return 0;
}
