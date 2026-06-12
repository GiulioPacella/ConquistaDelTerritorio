#include "common.h"     /* PRIMO: feature-test macro */
#include "protocol.h"
#include "net_util.h"
#include "game.h"
#include "users.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/socket.h>

/*
 * server.c — server concorrente a singolo processo con select().
 *
 * Un unico loop gestisce: accettazione connessioni, lettura comandi, scrittura
 * (coda di output per client), broadcast periodico e timeout globale della
 * partita. Tutti i socket sono non bloccanti. Nessun fork/thread/IPC: lo stato
 * è in normali strutture in memoria del processo.
 *
 * Vincolo FD_SETSIZE: gli fd devono restare sotto FD_SETSIZE (1024), perché
 * FD_SET su un fd >= FD_SETSIZE è undefined behavior. MAX_CLIENT è portato
 * vicino a quel tetto (1000) per onorare il piu' possibile il "nessun limite a
 * priori" della traccia; oltre, le connessioni vengono rifiutate.
 *
 * Il server NON scrive su stdout e NON legge da stdin; usa stderr solo per
 * errori fatali in fase di avvio.
 */

/* ===== Stato globale del server ===== */
static Giocatore giocatori[MAX_CLIENT];
static Mappa     partita;
static int       listen_fd = -1;
static time_t    ultimo_broadcast;
static unsigned int seed_base;
static int       seed_fisso;                 /* 1 se il seed è fissato da riga di comando */
static volatile sig_atomic_t fermati = 0;    /* impostato dai segnali per uscire pulito   */

/* ===== Gestione segnali ===== */
static void on_segnale(int s) {
    (void)s;
    fermati = 1;
}

/* ===== Helper di risposta ===== */
static void srv_ok(Giocatore *g, const char *msg) {
    char r[128];
    if (msg != NULL && msg[0] != '\0')
        snprintf(r, sizeof r, "%s %s\n", REP_OK, msg);
    else
        snprintf(r, sizeof r, "%s\n", REP_OK);
    coda_accoda_str(&g->out, r);
}

static void srv_err(Giocatore *g, const char *msg) {
    char r[160];
    snprintf(r, sizeof r, "%s %s\n", REP_ERR, msg);
    coda_accoda_str(&g->out, r);
}

/* ===== Gestione slot/giocatori ===== */
static void slot_init(int i) {
    memset(&giocatori[i], 0, sizeof giocatori[i]);
    giocatori[i].attivo = 0;
    giocatori[i].fd = -1;
    giocatori[i].id = i;
    giocatori[i].stato = ST_CONNESSO;
}

static void rimuovi_client(int i) {
    if (!giocatori[i].attivo) return;
    close(giocatori[i].fd);
    coda_free(&giocatori[i].out);
    /* Le celle conquistate da questo id restano sulla mappa (proprietà
       pubblica e persistente fino a ribaltamento o azzeramento partita). */
    slot_init(i);
}

/* Avvia una nuova partita: genera la mappa, fa spawnare tutti i loggati e
   azzera il loro fog-of-war. */
static void avvia_nuova_partita(void) {
    unsigned int seed = seed_fisso ? seed_base : (unsigned int)time(NULL);
    gioco_avvia(&partita, seed);
    ultimo_broadcast = time(NULL);
    for (int i = 0; i < MAX_CLIENT; i++) {
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO) {
            memset(giocatori[i].scoperto, 0, sizeof giocatori[i].scoperto);
            gioco_spawn(&partita, &giocatori[i].x, &giocatori[i].y);
            partita.proprieta[giocatori[i].y][giocatori[i].x] = giocatori[i].id;
            gioco_rivela_fog(&partita, &giocatori[i]);
        }
    }
}

/* Termina la partita: invia GAMEOVER a tutti i loggati e azzera lo stato di
   gioco. I client restano connessi e loggati; una nuova partita ripartirà al
   primo MOVE/LOGIN successivo. */
static void termina_partita(void) {
    gioco_aggiorna_punteggi(&partita, giocatori, MAX_CLIENT);
    for (int i = 0; i < MAX_CLIENT; i++)
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO)
            invia_gameover(&giocatori[i].out, &partita, giocatori, MAX_CLIENT);
    gioco_termina(&partita);
}

/* Invia la mappa globale a tutti i loggati (broadcast periodico). */
static void broadcast_global(void) {
    gioco_aggiorna_punteggi(&partita, giocatori, MAX_CLIENT);
    for (int i = 0; i < MAX_CLIENT; i++)
        if (giocatori[i].attivo && giocatori[i].stato == ST_IN_GIOCO)
            invia_global(&giocatori[i].out, &partita, giocatori, MAX_CLIENT);
    ultimo_broadcast = time(NULL);
}

/* ===== Dispatch di un comando =====
 * Ritorna 0 per continuare, 1 se la connessione va chiusa (QUIT). */
static int gestisci_riga(int i, char *linea) {
    Giocatore *g = &giocatori[i];
    char *cmd = strtok(linea, " \t");
    if (cmd == NULL) return 0;   /* riga vuota o solo spazi: ignora */

    /* ---- QUIT: valido in ogni stato ---- */
    if (strcmp(cmd, CMD_QUIT) == 0) {
        srv_ok(g, "arrivederci");
        coda_drena(&g->out, g->fd);   /* best-effort prima di chiudere */
        return 1;
    }

    /* ---- REGISTER: solo da CONNESSO ---- */
    if (strcmp(cmd, CMD_REGISTER) == 0) {
        if (g->stato != ST_CONNESSO) { srv_err(g, "gia' loggato"); return 0; }
        char *nick = strtok(NULL, " \t");
        char *pass = strtok(NULL, " \t");
        if (nick == NULL || pass == NULL) { srv_err(g, "uso: REGISTER <nick> <pass>"); return 0; }
        if (strlen(nick) > MAX_NICK) { srv_err(g, "nickname troppo lungo"); return 0; }
        if (strlen(pass) > MAX_PASS) { srv_err(g, "password troppo lunga"); return 0; }
        int r = utenti_registra(nick, pass);
        if (r == 0)        srv_ok(g, "registrato");
        else if (r == -1)  srv_err(g, "nickname gia' esistente");
        else               srv_err(g, "registrazione fallita");
        return 0;
    }

    /* ---- LOGIN: solo da CONNESSO ---- */
    if (strcmp(cmd, CMD_LOGIN) == 0) {
        if (g->stato != ST_CONNESSO) { srv_err(g, "gia' loggato"); return 0; }
        char *nick = strtok(NULL, " \t");
        char *pass = strtok(NULL, " \t");
        if (nick == NULL || pass == NULL) { srv_err(g, "uso: LOGIN <nick> <pass>"); return 0; }
        if (!utenti_verifica(nick, pass)) { srv_err(g, "credenziali errate"); return 0; }
        /* il nick non deve essere già online */
        for (int k = 0; k < MAX_CLIENT; k++)
            if (giocatori[k].attivo && giocatori[k].stato == ST_IN_GIOCO &&
                strcmp(giocatori[k].nick, nick) == 0) {
                srv_err(g, "utente gia' online");
                return 0;
            }
        /* login riuscito */
        g->stato = ST_IN_GIOCO;
        snprintf(g->nick, sizeof g->nick, "%s", nick);
        memset(g->scoperto, 0, sizeof g->scoperto);

        if (partita.stato != PARTITA_ATTIVA) {
            avvia_nuova_partita();    /* spawna anche questo giocatore */
        } else {
            gioco_spawn(&partita, &g->x, &g->y);
            partita.proprieta[g->y][g->x] = g->id;
            gioco_rivela_fog(&partita, g);
        }

        char msg[64];
        snprintf(msg, sizeof msg, "login id=%d", g->id);
        srv_ok(g, msg);
        gioco_aggiorna_punteggi(&partita, giocatori, MAX_CLIENT);
        invia_local(&g->out, &partita, g);    /* torcia: illumina i dintorni gia' allo spawn */
        invia_global(&g->out, &partita, giocatori, MAX_CLIENT);
        return 0;
    }

    /* ---- MOVE: solo IN_GIOCO ---- */
    if (strcmp(cmd, CMD_MOVE) == 0) {
        if (g->stato != ST_IN_GIOCO) { srv_err(g, "devi prima fare LOGIN"); return 0; }
        char *d = strtok(NULL, " \t");
        if (d == NULL || d[1] != '\0') { srv_err(g, "uso: MOVE <U|D|L|R>"); return 0; }
        char dir = (char)toupper((unsigned char)d[0]);

        if (partita.stato != PARTITA_ATTIVA)
            avvia_nuova_partita();     /* riparte una nuova partita */

        int r = gioco_move(&partita, g, dir);
        switch (r) {
            case 0:  invia_local(&g->out, &partita, g); break;
            case 1:  srv_err(g, "muro"); break;
            case 2:  srv_err(g, "fuori dai bordi"); break;
            default: srv_err(g, "direzione non valida (usa U/D/L/R)"); break;
        }
        return 0;
    }

    /* ---- WHO: solo IN_GIOCO ---- */
    if (strcmp(cmd, CMD_WHO) == 0) {
        if (g->stato != ST_IN_GIOCO) { srv_err(g, "devi prima fare LOGIN"); return 0; }
        invia_users(&g->out, giocatori, MAX_CLIENT);
        return 0;
    }

    /* ---- MAP: solo IN_GIOCO ---- */
    if (strcmp(cmd, CMD_MAP) == 0) {
        if (g->stato != ST_IN_GIOCO) { srv_err(g, "devi prima fare LOGIN"); return 0; }
        gioco_aggiorna_punteggi(&partita, giocatori, MAX_CLIENT);
        invia_global(&g->out, &partita, giocatori, MAX_CLIENT);
        return 0;
    }

    srv_err(g, "comando sconosciuto");
    return 0;
}

/* ===== Accettazione nuove connessioni ===== */
static void accetta_connessioni(void) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;                 /* riprova */
            break;  /* EAGAIN/EWOULDBLOCK: nessun'altra connessione pronta */
        }
        /* select() gestisce solo fd < FD_SETSIZE: oltre, FD_SET sarebbe
           undefined behavior. Rifiuta la connessione se l'fd supera il tetto
           di sistema. */
        if (fd >= FD_SETSIZE) {
            const char *m = REP_ERR " server pieno (limite di sistema)\n";
            send(fd, m, strlen(m), MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        /* cerca uno slot libero */
        int slot = -1;
        for (int i = 0; i < MAX_CLIENT; i++)
            if (!giocatori[i].attivo) { slot = i; break; }
        if (slot < 0) {
            /* tutti gli slot in uso: avvisa e chiudi */
            const char *m = REP_ERR " server pieno\n";
            send(fd, m, strlen(m), MSG_NOSIGNAL);
            close(fd);
            continue;
        }
        if (imposta_nonblocking(fd) < 0) { close(fd); continue; }
        slot_init(slot);
        giocatori[slot].attivo = 1;
        giocatori[slot].fd = fd;
        giocatori[slot].stato = ST_CONNESSO;
        buf_in_init(&giocatori[slot].in);
        coda_init(&giocatori[slot].out);
    }
}

/* ===== Lettura dati da un client ===== */
static void servi_lettura(int i) {
    char tmp[2048];
    ssize_t n = read(giocatori[i].fd, tmp, sizeof tmp);
    if (n > 0) {
        buf_in_aggiungi(&giocatori[i].in, tmp, (size_t)n);
        char linea[MAX_LINE + 1];
        int r;
        while ((r = buf_in_estrai_riga(&giocatori[i].in, linea, sizeof linea)) != 0) {
            if (r == -1) {
                srv_err(&giocatori[i], "riga troppo lunga");
                continue;
            }
            if (gestisci_riga(i, linea) == 1) {  /* QUIT */
                rimuovi_client(i);
                return;
            }
        }
    } else if (n == 0) {
        rimuovi_client(i);                        /* disconnessione ordinata */
    } else {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;                               /* non pronto: riprova dopo */
        rimuovi_client(i);                        /* errore (es. RST): rimuovi */
    }
}

/* ===== Pulizia finale ===== */
static void chiudi_tutto(void) {
    for (int i = 0; i < MAX_CLIENT; i++)
        if (giocatori[i].attivo) rimuovi_client(i);
    if (listen_fd >= 0) close(listen_fd);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "uso: %s <porta> [seed]\n", argv[0]);
        return 1;
    }
    int porta = atoi(argv[1]);
    if (porta < 1 || porta > 65535) {
        fprintf(stderr, "porta non valida: %s\n", argv[1]);
        return 1;
    }
    if (argc == 3) {
        seed_base = (unsigned int)strtoul(argv[2], NULL, 10);
        seed_fisso = 1;
    }

    for (int i = 0; i < MAX_CLIENT; i++) slot_init(i);

    /* segnali: SIGPIPE ignorato; SIGINT/SIGTERM → shutdown pulito */
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_segnale;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    listen_fd = crea_listening_socket(porta);
    if (listen_fd < 0) {
        fprintf(stderr, "impossibile creare il socket in ascolto sulla porta %d\n", porta);
        return 1;
    }

    partita.stato = PARTITA_FERMA;
    ultimo_broadcast = time(NULL);

    /* ===== Loop principale ===== */
    while (!fermati) {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        for (int i = 0; i < MAX_CLIENT; i++) {
            if (!giocatori[i].attivo) continue;
            FD_SET(giocatori[i].fd, &rfds);
            if (!coda_vuota(&giocatori[i].out))
                FD_SET(giocatori[i].fd, &wfds);
            if (giocatori[i].fd > maxfd) maxfd = giocatori[i].fd;
        }

        /* timeout: quanto manca al prossimo broadcast / alla fine partita */
        struct timeval tv, *ptv = NULL;
        if (partita.stato == PARTITA_ATTIVA) {
            int al_broadcast = T_BROADCAST - (int)(time(NULL) - ultimo_broadcast);
            if (al_broadcast < 0) al_broadcast = 0;
            int al_fine = gioco_secondi_residui(&partita);
            int w = (al_broadcast < al_fine) ? al_broadcast : al_fine;
            tv.tv_sec = w;
            tv.tv_usec = 0;
            ptv = &tv;
        }

        int pronti = select(maxfd + 1, &rfds, &wfds, NULL, ptv);
        if (pronti < 0) {
            if (errno == EINTR) continue;   /* segnale: riprova (o esci se fermati) */
            break;                          /* errore fatale */
        }

        /* --- compiti periodici basati sul tempo --- */
        if (partita.stato == PARTITA_ATTIVA) {
            if (gioco_scaduta(&partita)) {
                termina_partita();
            } else if ((int)(time(NULL) - ultimo_broadcast) >= T_BROADCAST) {
                broadcast_global();
            }
        }

        if (pronti == 0) continue;          /* solo timeout: nient'altro da fare */

        /* --- nuove connessioni --- */
        if (FD_ISSET(listen_fd, &rfds))
            accetta_connessioni();

        /* --- attività sui client --- */
        for (int i = 0; i < MAX_CLIENT; i++) {
            if (!giocatori[i].attivo) continue;
            if (FD_ISSET(giocatori[i].fd, &rfds)) {
                servi_lettura(i);
                if (!giocatori[i].attivo) continue;  /* potrebbe essere stato rimosso */
            }
            if (FD_ISSET(giocatori[i].fd, &wfds)) {
                if (coda_drena(&giocatori[i].out, giocatori[i].fd) < 0)
                    rimuovi_client(i);
            }
        }
    }

    chiudi_tutto();
    return 0;
}
