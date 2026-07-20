
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


// array di 1000 slot per i giocatori, ogni casella può contenere un giocatore connesso o essere vuota
// è la struct giocatore che tiee traccia dello stato della connessione (giocatore.attivo = 1 se lo slot è occupato dal client, giocatore.attivo = 0 se è libero)
static Giocatore giocatori[MAX_CLIENT];
static Mappa     partita;
static int       listen_fd = -1;
static time_t    ultimo_broadcast;
static unsigned int seed_base;
static int       seed_fisso;                 /* 1 se il seed è fissato da riga di comando */
static volatile sig_atomic_t fermati = 0;    /* impostato dai segnali per uscire pulito   */


// Gestione segnali

// mette il flag fermati a 1, che indica al server di uscire pulito dal loop principale
static void on_segnale(int s) {
    (void)s; // serve solo a usare il parametro per non generale il warning "unused parameter", ma in realtà non mi interessa quale segnale è arrivato, voglio solo uscire pulito
    fermati = 1;
}

// Invia un messaggio di OK al client g, con eventuale messaggio aggiuntivo msg
static void srv_ok(Giocatore *g, const char *msg) {
    char r[128];
    if (msg != NULL && msg[0] != '\0')
        snprintf(r, sizeof r, "%s %s\n", REP_OK, msg);
    else
        snprintf(r, sizeof r, "%s\n", REP_OK);
    coda_accoda_str(&g->out, r);
}

// Invia un messaggio di ERR al client g, con messaggio di errore msg
static void srv_err(Giocatore *g, const char *msg) {
    char r[160];
    snprintf(r, sizeof r, "%s %s\n", REP_ERR, msg);
    coda_accoda_str(&g->out, r);
}


// Gestione giocatori

// Azzera e reimposta lo slot numero i dell'array dei giocatori, rendendolo pronto per essere riusato
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
    /* Le celle conquistate da questo id restano sulla mappa fino alla prossima partita, ma il giocatore non è più loggato. */
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
   primo MOVE/LOGIN da dopo il T_PAUSA. */
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

// Gestione comandi ricevuti dai client
// Ritorna 0 per continuare, 1 se la connessione va chiusa (QUIT)
static int gestisci_riga(int i, char *linea) {
    Giocatore *g = &giocatori[i];
    char *cmd = strtok(linea, " \t");
    if (cmd == NULL) return 0;   /* riga vuota o solo spazi: ignora */

    /* QUIT: valido in ogni stato */
    if (strcmp(cmd, CMD_QUIT) == 0) {
        srv_ok(g, "arrivederci");
        coda_drena(&g->out, g->fd);   /* best-effort prima di chiudere */
        return 1;
    }

    /* REGISTER: solo da CONNESSO */
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

    // LOGIN: solo da CONNESSO
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
            /* Durante la pausa post-GAMEOVER nemmeno un nuovo login fa ripartire
               la partita: altrimenti chi entra in quel momento cancellerebbe la
               classifica agli altri. Resta loggato in attesa e verrà spawnato
               da avvia_nuova_partita al primo MOVE utile. */
            if (gioco_pausa_residua(&partita) == 0)
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
        invia_global(&g->out, &partita, giocatori, MAX_CLIENT);
        return 0;
    }

    // MOVE: solo IN_GIOCO */
    if (strcmp(cmd, CMD_MOVE) == 0) {
        if (g->stato != ST_IN_GIOCO) { srv_err(g, "devi prima fare LOGIN"); return 0; }
        char *d = strtok(NULL, " \t");
        if (d == NULL || d[1] != '\0') { srv_err(g, "uso: MOVE <U|D|L|R>"); return 0; }
        char dir = (char)toupper((unsigned char)d[0]);

        if (partita.stato != PARTITA_ATTIVA) {
            /* Pausa post-GAMEOVER: il movimento non riavvia nulla, così la
               classifica resta leggibile invece di essere spazzata via dal
               primo tasto premuto. */
            int attesa = gioco_pausa_residua(&partita);
            if (attesa > 0) {
                char msg[80];
                snprintf(msg, sizeof msg,
                         "partita finita: nuova partita fra %d s", attesa);
                srv_err(g, msg);
                return 0;
            }
            avvia_nuova_partita();     /* riparte una nuova partita */
        }

        int r = gioco_move(&partita, g, dir);
        switch (r) {
            case 0:  invia_local(&g->out, &partita, g); break;
            case 1:  srv_err(g, "muro"); break;
            case 2:  srv_err(g, "fuori dai bordi"); break;
            default: srv_err(g, "direzione non valida (usa U/D/L/R)"); break;
        }
        return 0;
    }

    /* WHO: solo IN_GIOCO */
    if (strcmp(cmd, CMD_WHO) == 0) {
        if (g->stato != ST_IN_GIOCO) { srv_err(g, "devi prima fare LOGIN"); return 0; }
        invia_users(&g->out, giocatori, MAX_CLIENT);
        return 0;
    }

    srv_err(g, "comando sconosciuto");
    return 0;
}

// Accettazione nuove connessioni 
static void accetta_connessioni(void) {
    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;                 /* riprova */
            break;  /* EAGAIN/EWOULDBLOCK: nessun'altra connessione pronta */
        }
        /* cerca uno slot libero */
        int slot = -1;
        for (int i = 0; i < MAX_CLIENT; i++)
            if (!giocatori[i].attivo) { slot = i; break; }
        if (slot < 0) {
            /* server pieno: avvisa e chiudi */
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

// Lettura dati da un client
static void servi_lettura(int i) {
    // buffer temporaneo per leggere i dati dal socket, di dimensione 2048 byte
    char tmp[2048];
    // read legge i dati dal socket del client e li mette nel buffer tmp, fino a sizeof(tmp) byte 
    // e restituisce il numero di byte letti, 0 se il client ha chiuso la connessione, -1 se c'è stato un errore
    ssize_t n = read(giocatori[i].fd, tmp, sizeof tmp);
    if (n > 0) {
        buf_in_aggiungi(&giocatori[i].in, tmp, (size_t)n);
        char linea[MAX_LINE + 1];
        int r;
        // estrai riga restituisce 1 se ha estratto una riga completa, 0 se non c'è una riga completa, -1 se la riga è troppo lunga e va scartata
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
        // vuol dire che il client ha chiuso la connessione, quindi rimuovo il client dallo slot
        rimuovi_client(i);                        /* disconnessione ordinata */
    } else {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return;                               /* non pronto: riprova dopo */
        rimuovi_client(i);                        /* errore (es. RST): rimuovi */
    }
}

static void chiudi_tutto(void) {
    for (int i = 0; i < MAX_CLIENT; i++)
        if (giocatori[i].attivo) rimuovi_client(i);
    if (listen_fd >= 0) close(listen_fd);
}

int main(int argc, char *argv[]) {
    
    // Controlla che il numero di argomenti sia valido 
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "uso: %s <porta> [seed]\n", argv[0]);
        return 1;
    }
    // Atoi = ASCII to int 
    int porta = atoi(argv[1]);
    
    // Controlla la validità della porta 
    if (porta < 1 || porta > 65535) {
        fprintf(stderr, "porta non valida: %s\n", argv[1]);
        return 1;
    }

    // Se è stato fornito un seed, lo converte in unsigned int (string to unsigned int)e imposta seed_fisso a 1. 
    // NULL = dove salvare dove si ferma la conversione (null implica da nessuna parte), 10 = base decimale
    if (argc == 3) {
        seed_base = (unsigned int)strtoul(argv[2], NULL, 10);
        seed_fisso = 1;
    }

    // Azzera e reimposta tutti gli slot dei giocatori, rendendoli pronti per essere riusati
    for (int i = 0; i < MAX_CLIENT; i++) slot_init(i);

    // Ignora il segnale SIGPIPE, che viene generato quando si tenta di scrivere su un socket chiuso dal peer 
    //(il client si è disconnesso e il server continua a mandargli roba)
    signal(SIGPIPE, SIG_IGN);
    
    // la struct sigaction è una shceda di configurazione che descrivi in modo personalizzato per dire
    // al sistema operativo come gestire un segnale
    struct sigaction sa;
    
    // metto a 0 tutti i campi che contengono valori spazzatura (quello che c'era in memoria) che non mi interessano, come il campo sa_mask 
    //(quali segnali bloccare) e sa_flags. mettendoli a 0 gli dico di usare i valori di default
    memset(&sa, 0, sizeof sa);

    // imposto il campo sa_handler, che descrive la funzione da chiamare quando il segnale arriva, a on_segnale, che è la funzione che ho definito sopra
    sa.sa_handler = on_segnale;
    
    // le sigaction consegnano le schede di configurazioni struct al sistema, primo argomento quale segnale voglio gestire,
    // secondo argomento la struct che descrive come gestirlo, terzo argomento è una struct che il sistema può usare per restituire la vecchia configurazione
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // restituisce l'fd della socket passiva in ascolto sulle nuove connessioni 
    listen_fd = crea_listening_socket(porta);
    if (listen_fd < 0) {
        fprintf(stderr, "impossibile creare il socket in ascolto sulla porta %d\n", porta);
        return 1;
    }

    partita.stato = PARTITA_FERMA;
    partita.fine  = 0;            /* nessun GAMEOVER ancora: nessuna pausa iniziale */
    ultimo_broadcast = time(NULL);

    // Loop principale 
    while (!fermati) {
        // preparo i set di file descriptor da passare a select: rfds = read fds, wfds = write fds
        fd_set rfds, wfds;
        
        // ogni volta che viene usata la selct, essa distrugge il contenuto dei set restituendo solo i pronti, quindi ogni giro vanno riscritti da zero
        // tramite FD_ZERO, che li azzera, e poi FD_SET, che aggiunge i file descriptor da monitorare
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        
        // passo a rfds la socket in ascolto, che è quella che accetta le nuove connessioni, quindi in questo caso se la select mi dice che 
        // è pronta, significa che c'è una nuova connessione in arrivo e posso chiamare accept per accettarla
        FD_SET(listen_fd, &rfds);
        
        // maxfd = il file descriptor più grande da passare a select, che è necessario perché select deve sapere quanti file descriptor deve controllare,
        // per ora lo imposto a quello di listen_fd perchè è l'unico fd
        int maxfd = listen_fd;

        // quando il server vuole fare qualcosa per tutti i client non ha un elenco separato dei soli attivi, ha solo l'array di 1000 slot (MAX_CLIENT)
        // dove alcuni sono occupati e altri vuoti sparsi ovunque, quindi scorre tutte le caselle
        for (int i = 0; i < MAX_CLIENT; i++) {
            // salta le caselle vuote (giocatori[i].attivo = 0)
            if (!giocatori[i].attivo) continue;
            // aggiunge il file descriptor del client attivi al set di lettura, 
            // in modo che select possa monitorarlo e dirmi quando ci sono dati pronti da leggere
            FD_SET(giocatori[i].fd, &rfds);
            
            // aggiunge il file descriptor del client attivo al set di scrittura, solo se la coda di output non è vuota,
            // quindi solo se ho effettivamente qualcosa da mandargli, non quando è semplicemente pronto a ricevere dati
            // così select può addormentarsi tranquillo finchè non c'è davvero lavoro da fare
            if (!coda_vuota(&giocatori[i].out))
                FD_SET(giocatori[i].fd, &wfds);
            
            // ricontrollo maxfd per essere sicuro che sia sempre il più grande tra i fd aggiunti ai set
            if (giocatori[i].fd > maxfd) maxfd = giocatori[i].fd;
        }

        /* timeout: quanto manca al prossimo broadcast / alla fine partita */
        
        // timeval che specifica alla select quanto tempo deve aspettare prima di svegliarsi
        // se si svegliasse solo quando ci sono socket pronti (con NULL) non farebbe mai il broadcast periodico o la fine partita, quindi devo dargli un timeout
        // ne inizializzo 2 perchè uno lo devo modificare e l'altro lo devo passare a select come puntatore (poi li associo alla fine)
        struct timeval tv, *ptv = NULL;
        
        // isolo durante una partita servono le scadenze. Se la partita è ferma allora ptv resta NULL e select si sveglia solo quando ci sono socket pronti
        if (partita.stato == PARTITA_ATTIVA) {
            // calcola quanto manca al prossimo broadcast, che è T_BROADCAST secondi dopo l'ultimo broadcast
            int al_broadcast = T_BROADCAST - (int)(time(NULL) - ultimo_broadcast);
            // se il momento è già passato, lo imposto a 0 così select si sveglia subito
            if (al_broadcast < 0) al_broadcast = 0;
            // calcola quanto manca alla fine della partita
            int al_fine = gioco_secondi_residui(&partita);
            // scegli il tempo minore tra i due
            int w = (al_broadcast < al_fine) ? al_broadcast : al_fine;
            // imposta i secondi del timeval al tempo minore calcolato (e i microsecondi a 0 perchè non mi interessano)
            tv.tv_sec = w;
            tv.tv_usec = 0;
            // ora che l'ho modificato riassegnandolo al puntatore posso passarlo poi a select 
            ptv = &tv;
        }

        // partita ferma = ptv == NULL = select si sveglia solo quando ci sono socket pronti, partita attiva = ptv != NULL = select si sveglia anche quando scade il timeout
        // select restituisce il numero di socket pronti, 0 se è scaduto il timeout senza socket pronti, -1 se c'è stato un errore
        int pronti = select(maxfd + 1, &rfds, &wfds, NULL, ptv);
        // indica che select ha fallito, in quel caso select imposta errno a un valore che indica il tipo di errore
        // se errno == EINTR significa che select è stata interrotta da un segnale, quindi riprovo a chiamarla, altrimenti esco dal loop perchè c'è stato un errore fatale
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

        // se select è scaduta senza socket pronti, non c'è altro da fare tranne i compiti periodici fatti sopra, quindi ricomincia il loop
        if (pronti == 0) continue;          /* solo timeout: nient'altro da fare */

        /* --- nuove connessioni --- */
        // visto che select rimuove dai fd_set i file descriptor che non sono pronti, se listen_fd è ancora presente in rfds
        // significa che c'è una nuova connessione pronta da accettare
        if (FD_ISSET(listen_fd, &rfds))
            accetta_connessioni();

        /* --- attività sui client --- */
        // scorre tutti i giocatori
        for (int i = 0; i < MAX_CLIENT; i++) {
            // se il giocatore non è attivo, salta lo slot
            if (!giocatori[i].attivo) continue;
            // se il fd del giocatore è pronto in lettura, allora servi_lettura
            // servi_lettura però può aver rimosso il client durante l'elaborazione, se per esempio ha ricevuto un QUIT o ha letto un EOF(disconnessione)
            // quindi se dopo la lettura il giocatore non è più attivo, salta al prossimo slot
            if (FD_ISSET(giocatori[i].fd, &rfds)) {
                servi_lettura(i);
                if (!giocatori[i].attivo) continue;  /* potrebbe essere stato rimosso */
            }
            // se il fd è pronto in scrittura
            if (FD_ISSET(giocatori[i].fd, &wfds)) {
                if (coda_drena(&giocatori[i].out, giocatori[i].fd) < 0)
                    rimuovi_client(i);
            }
        }
    }

    chiudi_tutto();
    return 0;
}
