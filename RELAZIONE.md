# Relazione tecnica — "Conquista del territorio"

Sistema client/server in C su Linux/UNIX per un gioco multigiocatore a conquista
di celle su griglia, con comunicazione via socket TCP. Solo system call UNIX e
libreria standard del C; concorrenza realizzata con `select()` (singolo processo,
I/O multiplexing), senza fork, thread o memoria condivisa.

---

## Indice

1. [Architettura generale](#1-architettura-generale)
2. [Perché `select()` e non fork/thread](#2-perché-select-e-non-forkthread)
3. [Il protocollo applicativo (con esempi reali)](#3-il-protocollo-applicativo-con-esempi-reali)
4. [Strutture dati](#4-strutture-dati)
5. [Il loop del server passo per passo](#5-il-loop-del-server-passo-per-passo)
6. [Gestione dell'I/O non bloccante](#6-gestione-dellio-non-bloccante)
7. [Scelte progettuali](#7-scelte-progettuali)
8. [Robustezza e casi limite gestiti](#8-robustezza-e-casi-limite-gestiti)
9. [Il client](#9-il-client)
10. [Compilazione, esecuzione e verifica](#10-compilazione-esecuzione-e-verifica)

---

## 1. Architettura generale

```
   ┌──────────────┐                         ┌──────────────────────────────┐
   │   CLIENT 1   │── TCP ───┐              │            SERVER            │
   └──────────────┘          │              │  (1 processo, select())      │
   ┌──────────────┐          ├──────────────┤  - conosce TUTTA la mappa     │
   │   CLIENT 2   │── TCP ───┤   socket     │  - valida ogni comando        │
   └──────────────┘          │              │  - aggiorna lo stato di gioco │
   ┌──────────────┐          │              │  - invia LOCAL/GLOBAL/...     │
   │   CLIENT N   │── TCP ───┘              │  - persiste gli account       │
   └──────────────┘                         └──────────────────────────────┘
```

Il server è **l'unica fonte di verità**: i client sono volutamente "stupidi",
inoltrano i comandi dell'utente e disegnano ciò che ricevono. Questo concentra la
logica in un solo punto, elimina la possibilità di imbrogli e semplifica il codice
del client.

Il progetto è **modulare**:

| Modulo            | Responsabilità                                                 |
|-------------------|----------------------------------------------------------------|
| `common.h`        | parametri, limiti, feature-test macro, enum di stato           |
| `protocol.h`      | costanti e documentazione del protocollo, simboli delle mappe  |
| `net_util.[ch]`   | socket, buffer di input (framing a righe), coda di output      |
| `users.[ch]`      | registrazione/verifica account, persistenza su `users.dat`     |
| `game.[ch]`       | mappa, mosse, fog-of-war, punteggi, serializzazione messaggi   |
| `server.c`        | orchestrazione: loop `select()`, dispatch, broadcast, segnali  |
| `client.c`        | UI interattiva non bloccante e rendering ASCII/colori          |

---

## 2. Perché `select()` e non fork/thread

Il server deve servire molti client **contemporaneamente**. Una `read()` bloccante
su un client congelerebbe il processo ignorando tutti gli altri. Tre approcci
classici:

- **un processo/thread per client** (`fork`/`pthread`): semplice da scrivere ma
  introduce concorrenza reale sullo stato condiviso (mappa, proprietà), quindi
  servono mutex/IPC/memoria condivisa, con rischio di *race condition*;
- **`select()` (scelto qui)**: un solo processo aspetta su **tutti** i socket
  insieme e viene risvegliato solo su quelli pronti. Vantaggi:
  1. **nessuna race condition**: c'è un unico flusso di esecuzione, quindi mentre
     si elabora la mossa di un giocatore nessun altro tocca lo stato — niente
     mutex, niente IPC;
  2. **stato condiviso gratis**: mappa e giocatori sono normali variabili del
     processo;
  3. **timer integrato**: il `timeout` di `select()` fornisce il "battito" per il
     broadcast periodico e per il controllo del timeout globale, senza thread
     timer dedicati;
  4. **scalabilità sufficiente**: con `FD_SETSIZE = 1024` e `MAX_CLIENT = 64`
     siamo ampiamente entro i limiti.

Lo svantaggio teorico (un client lentissimo potrebbe rallentare gli altri) è
neutralizzato rendendo **tutti i socket non bloccanti** e **accodando l'output**:
nessuna operazione di I/O blocca mai il loop.

---

## 3. Il protocollo applicativo (con esempi reali)

Protocollo **testuale e linea-orientato**: ogni messaggio è una riga terminata da
`\n`. È leggibile e testabile a mano con `nc`. Poiché TCP è un flusso di byte (le
`read()` possono spezzare o aggregare i messaggi), entrambi i lati accumulano i
byte in un buffer ed estraggono **una riga completa per volta** (*framing*).

### Client → Server
`REGISTER <nick> <pass>` · `LOGIN <nick> <pass>` · `MOVE <U|D|L|R>` · `WHO` ·
`MAP` · `QUIT`

### Server → Client
`OK [msg]` · `ERR <msg>` · `LOCAL …` · `GLOBAL …` · `USERS …` · `GAMEOVER …`
(gli ultimi quattro sono multi-riga e terminano con `END`).

### Simboli delle celle
`#` muro scoperto · `.` cella libera · `0–9`/`a–z` cella posseduta dal giocatore
con quell'id · `@` posizione del giocatore (solo `LOCAL`) · `?` fuori mappa/ignoto.

### Esempi reali (catturati durante i test)

Login e prima mappa globale:

```
C: REGISTER mario segreto
S: OK registrato
C: LOGIN mario segreto
S: OK login id=0
S: GLOBAL 30 30 1
   ..............................
   ...(altre righe)...
   ............0.................
   ..............................
   P 0 mario 12 27 1
   END
```

Una mossa valida produce la mappa locale (finestra 7×7 centrata su `@`):

```
C: MOVE U
S: LOCAL 12 26 3
   ..##.#.
   ..#....
   ...##..
   ...@..#
   ...0.#.        ← '0' = cella appena lasciata, ora di proprietà di mario
   .#....#
   .......
   END
```

Mossa contro un muro e WHO:

```
C: MOVE R
S: ERR muro       ← il muro entra nel fog-of-war e comparirà nei LOCAL successivi
C: WHO
S: USERS 1
   0 mario
   END
```

Fine partita:

```
S: GAMEOVER 1
   1 mario 3
   END
```

### Serializzazione (esatta)

- **LOCAL**: `LOCAL <cx> <cy> <r>\n`, poi `2r+1` righe da `2r+1` caratteri, poi `END\n`.
- **GLOBAL**: `GLOBAL <W> <H> <nplayers>\n`, `H` righe da `W` caratteri (mappa
  proprietà, senza `@` e senza muri, che sono privati), `nplayers` righe
  `P <id> <nick> <x> <y> <score>\n`, poi `END\n`.
- **USERS**: `USERS <n>\n`, `n` righe `<id> <nick>\n`, poi `END\n`.
- **GAMEOVER**: `GAMEOVER <n>\n`, `n` righe `<pos> <nick> <score>\n` (ordine
  decrescente di punteggio), poi `END\n`.

---

## 4. Strutture dati

```c
/* Un giocatore = uno slot del server (game.h) */
typedef struct {
    int           attivo;          /* 1 = slot in uso                         */
    int           fd;              /* socket del client; -1 se libero         */
    StatoClient   stato;           /* ST_CONNESSO / ST_IN_GIOCO               */
    int           id;              /* id (= indice di slot)                   */
    char          nick[MAX_NICK + 1];
    int           x, y;            /* posizione corrente                      */
    int           punteggio;       /* celle possedute (ricalcolato)           */
    unsigned char scoperto[MAPPA_H][MAPPA_W]; /* fog-of-war privato            */
    BufferIn      in;              /* buffer input: framing a righe           */
    CodaOut       out;             /* coda di output (write parziali/EAGAIN)  */
} Giocatore;

/* Lo stato del mondo (game.h) */
typedef struct {
    unsigned char muri[MAPPA_H][MAPPA_W];      /* 1 = muro (noto solo al server)*/
    int           proprieta[MAPPA_H][MAPPA_W]; /* id proprietario, -1 = nessuno */
    StatoPartita  stato;                       /* PARTITA_FERMA / PARTITA_ATTIVA*/
    time_t        inizio;                      /* istante di avvio partita      */
} Mappa;
```

Sul server: un array statico `Giocatore giocatori[MAX_CLIENT]` (l'id coincide con
l'indice, così resta stabile) e una `Mappa partita`. L'unica memoria **dinamica**
è il buffer della `CodaOut`, che cresce con `realloc` e viene liberato con
`coda_free` alla disconnessione e nello shutdown.

- `muri[][]` è **pubblica per il server ma privata per i giocatori**: ciascuno la
  scopre tramite `scoperto[][]` (1 = "ho già visto questo muro").
- `proprieta[][]` è **pubblica**: appare nelle mappe globali a tutti.

---

## 5. Il loop del server passo per passo

```c
while (!fermati) {
    costruisci readfds (listen + tutti i client) e writefds (solo client con coda piena);
    calcola il timeout = min(tempo al prossimo broadcast, tempo a fine partita);
    select(maxfd+1, &readfds, &writefds, NULL, timeout);

    /* compiti periodici basati sul tempo (anche su timeout) */
    se partita scaduta            -> termina_partita() (GAMEOVER + azzeramento);
    altrimenti se è ora           -> broadcast_global();

    se il listening socket è pronto       -> accetta_connessioni();
    per ogni client pronto in lettura     -> servi_lettura()  (read, framing, dispatch);
    per ogni client pronto in scrittura   -> coda_drena()     (svuota la coda);
}
chiudi_tutto();   /* close di tutti i socket + free di tutte le code */
```

Il `readfds`/`writefds` viene **ricostruito a ogni giro** perché `select()` lo
modifica. Un client è inserito in `writefds` **solo** se ha byte pendenti in coda:
questo è essenziale durante un broadcast verso molti client, perché si scrive solo
quando il socket è effettivamente scrivibile.

Il **timeout** è calcolato come "quanto manca al prossimo evento temporale": così
il broadcast resta regolare anche con molto traffico (i compiti periodici sono
controllati con un timestamp, non solo sul ramo `select()==0`).

---

## 6. Gestione dell'I/O non bloccante

### Buffer di input — *framing* a righe
Ogni client ha un `BufferIn`. Dopo ogni `read()` i byte grezzi sono accodati
(`buf_in_aggiungi`) e si estraggono le righe complete una alla volta
(`buf_in_estrai_riga`, che cerca `\n`, tollera `\r\n` e gli spazi multipli). Una
riga più lunga di `MAX_LINE` (1024) viene **scartata** con `ERR`, proteggendo da
input malevolo. Letture spezzate (`LOG` + `IN mario pwd\n`) e aggregate
(`WHO\nMAP\n`) sono gestite correttamente.

### Coda di output — write parziali ed EAGAIN
Con socket non bloccanti anche `send()` può scrivere parzialmente o restituire
`EAGAIN`. Ogni client ha una `CodaOut` dinamica: le risposte vengono **accodate**
(`coda_accoda`) e drenate quando il socket è scrivibile (`coda_drena`, che usa
`send(..., MSG_NOSIGNAL)`). Se l'invio è parziale o si ferma su `EAGAIN`, il resto
rimane in coda e si riprova al giro successivo; il fd viene tolto da `writefds`
quando la coda si svuota.

### Robustezza delle system call
`EINTR` è trattato come "riprova"; `EAGAIN`/`EWOULDBLOCK` come "non pronto, riprova
dopo". `accept()`, `read()`, `send()`, `select()` gestiscono questi casi senza mai
considerarli errori fatali.

---

## 7. Scelte progettuali

### Sistema di coordinate (fissato)
Origine `(0,0)` in alto a sinistra; `x` = colonna (`0..W-1`), `y` = riga
(`0..H-1`). Movimenti: `U → y-1`, `D → y+1`, `L → x-1`, `R → x+1`. Le mosse fuori
dai bordi sono rifiutate con `ERR`.

### Conquista / ribaltamento
Entrando in una cella libera, `proprieta[y][x]` viene impostata all'id del
giocatore: se la cella era di un altro, **cambia padrone** (ribaltamento). La
cella di spawn viene assegnata subito al giocatore (punteggio iniziale 1). I
punteggi sono **ricalcolati** scandendo la mappa prima di ogni `GLOBAL`/`GAMEOVER`,
così restano sempre coerenti anche dopo i ribaltamenti.

### Fog-of-war
I muri sono privati. A ogni mossa (e allo spawn) `gioco_rivela_fog` marca in
`scoperto[][]` i muri della finestra `(2·R_FOG+1)²` (default 7×7). Un muro è
mostrato in `LOCAL` **solo se** quel giocatore l'ha già scoperto; le scoperte si
**accumulano** per tutta la partita. Le proprietà delle celle restano invece
pubbliche.

### Ciclo di vita della partita
Il timer globale parte al **primo login**. Chi entra a partita in corso fa spawn
nella partita attiva. Allo scadere di `T_PARTITA` il server invia `GAMEOVER` con la
classifica a tutti i loggati, poi **azzera** lo stato (mappa, proprietà, posizioni,
fog) tornando in `PARTITA_FERMA`. I client **restano connessi e loggati**: una
nuova partita riparte automaticamente al primo `MOVE`/`LOGIN` successivo (che
rigenera la mappa e rifà spawnare tutti). I client sono utilizzabili senza riavvio.

### Macchina a stati del client (enforce sul server)
- `ST_CONNESSO`: ammessi solo `REGISTER`, `LOGIN`, `QUIT`;
- `ST_IN_GIOCO`: ammessi `MOVE`, `WHO`, `MAP`, `QUIT`.
Un comando valido ma nello stato sbagliato produce un `ERR` esplicativo
(es. `MOVE` prima del login → `ERR devi prima fare LOGIN`).

### Identità e id di slot
L'id del giocatore coincide con l'indice di slot, così è stabile e mappabile su un
singolo carattere (`simbolo_proprietario`). **Limite noto**: alla disconnessione le
celle conquistate restano sulla mappa con quell'id (proprietà persistente); se un
nuovo client riusa lo stesso slot ne eredita le celle. È una semplificazione
didattica e in ogni caso lo stato viene azzerato a fine partita.

### Account
Registrazione con nickname unico + password, persistiti su `users.dat`
(`nick pass` per riga). **Password in chiaro**: limite noto accettabile per lo
scopo didattico, segnalato nei commenti di `users.c`.

---

## 8. Robustezza e casi limite gestiti

| Caso                          | Comportamento                                                |
|-------------------------------|--------------------------------------------------------------|
| `SIGPIPE` (scrittura su peer chiuso) | ignorato (`SIG_IGN`) + `MSG_NOSIGNAL`: il server non muore |
| Disconnessione brusca (RST/`kill`)   | `read` ritorna 0/errore → rimozione pulita dello slot, server vivo |
| `SIGINT`/`SIGTERM`            | flag → uscita dal loop → `close` di tutti i socket + `free` (shutdown pulito) |
| Write parziale / `EAGAIN`     | il resto resta in coda, si riprende quando il socket è scrivibile |
| Lettura spezzata / aggregata  | buffer di input con framing a `\n`                            |
| Riga > `MAX_LINE`             | scartata con `ERR riga troppo lunga`                         |
| Nick duplicato in registrazione | `ERR nickname gia' esistente`                              |
| Login con nick già online     | `ERR utente gia' online`                                     |
| Comando in stato errato       | `ERR` esplicativo                                            |
| `Address already in use` al riavvio | `SO_REUSEADDR` sul listening socket                   |
| Mappa piena di muri (no spawn) | `gioco_spawn` ritorna -1 (gestito)                          |
| `EINTR` su select/accept/read | trattato come "riprova"/"non pronto"                         |

### Memoria
L'unica allocazione dinamica è il buffer della coda di output. Viene liberato in
`rimuovi_client` (disconnessione/QUIT/errore) e in `chiudi_tutto` (shutdown). Il
codice è quindi scritto per essere **valgrind-clean** nei percorsi normali e di
disconnessione:

```sh
valgrind --leak-check=full ./server 8080
```

---

## 9. Il client

`./client <host> <porta>` si connette con `connect()` (via `getaddrinfo`, accetta
hostname o IP) e usa `select()` su **due** sorgenti: la tastiera (`stdin`) e il
socket. Così l'utente digita comandi mentre i broadcast arrivano, senza bloccarsi.
Anche il client bufferizza l'input dal server e lo processa una riga per volta.

Il client riconosce i messaggi multi-riga accumulando le righe fino a `END` e poi
rendendole: la mappa locale e quella globale sono disegnate in ASCII con una
**legenda** e **colori ANSI opzionali** (attivati solo se `stdout` è un terminale;
in pipe/file restano simboli ASCII puri). Le scorciatoie `W A S D` sono tradotte in
`MOVE U/L/D/R`.

---

## 10. Compilazione, esecuzione e verifica

```sh
make                      # compila senza warning (-Wall -Wextra -pedantic -std=c11)
./server 8080             # avvio server (porta 8080)
./client 127.0.0.1 8080   # avvio client
```

### Test del protocollo a mano

```sh
nc 127.0.0.1 8080
REGISTER mario segreto
LOGIN mario segreto
MOVE R
WHO
QUIT
```

### Verifiche effettuate
- compilazione pulita con i flag richiesti; `make clean` ripristina;
- registrazione/login, persistenza su `users.dat` e login dopo riavvio del server;
- `WHO` con più client; rimozione pulita su disconnessione brusca (RST), server vivo;
- nick duplicato e comandi in stato errato → `ERR`;
- `MOVE` con conquista/ribaltamento, rivelazione muri, `LOCAL` coerente;
- broadcast `GLOBAL` periodico; `GAMEOVER` con classifica corretta allo scadere del
  timeout; ripartenza pulita della partita al `MOVE` successivo;
- partita end-to-end con il client reale (rendering `LOCAL`/`GLOBAL`), input mai
  bloccato durante i broadcast.
