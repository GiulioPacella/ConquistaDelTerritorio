# Come funziona il programma — Conquista del territorio

> Documento didattico pensato per uno studente del 3° anno di Informatica.
> Spiega **passo per passo** come funzionerà il sistema client–server, senza dare per
> scontato nulla. Il codice non esiste ancora: qui descriviamo *come sarà fatto* usando
> pseudo-codice e disegni ASCII.

---

## Indice

1. [Visione d'insieme](#1-visione-dinsieme)
2. [Concetti di rete che servono](#2-concetti-di-rete-che-servono)
3. [Il modello `select()` spiegato da zero](#3-il-modello-select-spiegato-da-zero)
4. [Avvio del server](#4-avvio-del-server)
5. [Ciclo di vita di una connessione](#5-ciclo-di-vita-di-una-connessione)
6. [Una mossa, dall'inizio alla fine](#6-una-mossa-dallinizio-alla-fine)
7. [Il fog-of-war](#7-il-fog-of-war)
8. [Il broadcast periodico](#8-il-broadcast-periodico)
9. [Fine partita](#9-fine-partita)
10. [Lato client](#10-lato-client)
11. [Le strutture dati](#11-le-strutture-dati)
12. [Glossario](#12-glossario)

---

## 1. Visione d'insieme

Il gioco è una **gara di conquista** su una griglia. Più giocatori si muovono su una mappa
fatta di celle. Quando un giocatore entra in una cella, **quella cella diventa sua**. Se poi
ci passa un altro giocatore, la cella cambia padrone ("ribaltamento"). Alla fine vince chi
possiede più celle.

C'è una complicazione interessante: i **muri** (gli ostacoli) **non sono noti in anticipo**.
Ogni giocatore li scopre solo esplorando ("fog-of-war", la nebbia di guerra). Quindi due
giocatori vedono mappe diverse degli ostacoli, ma vedono **tutti la stessa** mappa di chi
possiede cosa.

Chi fa cosa:

```
   ┌─────────────────────────────┐         ┌─────────────────────────────┐
   │           CLIENT            │         │           SERVER            │
   │  (uno per giocatore)        │  TCP    │  (uno solo, per tutti)      │
   │                             │ <─────> │                             │
   │ - legge i tasti dell'utente │         │ - conosce TUTTA la mappa    │
   │ - manda comandi al server   │         │ - decide cosa è valido      │
   │ - disegna le mappe ricevute │         │ - aggiorna lo stato di gioco│
   │ - NON conosce il gioco vero │         │ - manda aggiornamenti       │
   └─────────────────────────────┘         └─────────────────────────────┘
```

Regola d'oro: **il server è l'unico ad avere la verità**. Il client è "stupido": mostra ciò
che gli arriva e inoltra ciò che l'utente chiede. Questo evita imbrogli e tiene la logica in
un posto solo.

---

## 2. Concetti di rete che servono

### Socket TCP
Un **socket** è un "tubo" bidirezionale tra due programmi. Con TCP il tubo è **affidabile**
(i byte arrivano tutti e in ordine) e **orientato alla connessione** (prima ci si "connette",
poi si parla). Dal punto di vista del programma, un socket è un **file descriptor (fd)**: un
numero intero su cui fai `read()` e `write()` come fosse un file.

### TCP è un flusso di BYTE, non di messaggi
Questo è il concetto che confonde di più. Se il client fa:

```c
write(fd, "MOVE U\n", 7);
```

il server **non** è garantito di riceverlo con una sola `read()`. Potrebbe ricevere:

- `MOVE U\n` tutto insieme (caso fortunato), **oppure**
- `MOV` adesso e `E U\n` tra un istante (frammentazione), **oppure**
- `MOVE U\nWHO\n` insieme, se il client ha mandato due comandi di fila (aggregazione).

TCP garantisce solo che i byte arrivino **in ordine**, non *come* sono raggruppati.

### La soluzione: un protocollo "a righe" (framing)
Per sapere dove finisce un messaggio ci mettiamo d'accordo: **ogni messaggio finisce con
`\n`**. Il ricevente accumula i byte in un **buffer** e considera "un messaggio completo" solo
quando trova un `\n`. Questa tecnica si chiama **framing** (delimitazione dei messaggi).

```
buffer del client sul server: "MOV"          → nessun \n, aspetto altri byte
arriva "E U\nWH"                              → buffer = "MOVE U\nWH"
                                               → trovo \n: estraggo "MOVE U", processo
                                               → resta "WH" nel buffer, aspetto
arriva "O\n"                                  → buffer = "WHO\n"
                                               → trovo \n: estraggo "WHO", processo
```

Il nostro **protocollo applicativo** è quindi un insieme di righe di testo tipo
`MOVE U`, `LOGIN mario segreta`, `WHO`, `QUIT`. Semplice da scrivere e da debuggare (lo puoi
leggere a occhio).

---

## 3. Il modello `select()` spiegato da zero

### Il problema
Il server deve servire **tanti client insieme**. Se usasse una `read()` "normale"
(bloccante), si fermerebbe sul primo client finché *quello* non parla:

```c
read(client1, ...);   // il processo SI CONGELA qui finché client1 non manda qualcosa
read(client2, ...);   // client2 ignorato nel frattempo
```

Con un solo processo e un solo flusso di esecuzione, aspettare su uno significa ignorare
tutti gli altri. Inaccettabile.

### L'idea di `select()`
`select()` ribalta la domanda. Invece di "fammi leggere da *questo* socket (e bloccami)",
diciamo al kernel:

> "Ecco la lista di **tutti** i miei fd (i client + il socket in ascolto). Mettimi a dormire
> e **svegliami appena UNO QUALSIASI** ha qualcosa di pronto da leggere. E se non succede
> niente entro `T` secondi, svegliami comunque."

Mentre dorme, il processo **non consuma CPU**. Si sveglia solo quando c'è lavoro vero.
Quando `select()` ritorna, ci dice **quali** fd sono pronti; serviamo solo quelli, in fretta,
e torniamo a dormire.

### Perché basta UN SOLO processo (e niente thread)
1. **Nessuno aspetta nessuno**: il processo dorme su *tutti* i socket insieme.
2. **Ogni comando si serve in microsecondi**: aggiornare una posizione e una cella è
   velocissimo, quindi tutti i client sembrano serviti "insieme".
3. **Stato condiviso gratis e senza race condition**: c'è un solo flusso di esecuzione,
   quindi mentre processi la mossa di Mario *nessun altro* tocca la mappa. Niente mutex,
   niente memoria condivisa, niente IPC. Questo è il grande vantaggio.
4. **Il timer è gratis**: il timeout di `select()` ci dà il "battito" ogni `T` secondi per
   mandare gli aggiornamenti globali.

### Il loop principale (cuore del server)

```c
while (partita_in_corso) {
    fd_set readfds;
    FD_ZERO(&readfds);                       // svuota l'insieme
    FD_SET(listen_fd, &readfds);             // voglio sapere se arrivano NUOVE connessioni
    int maxfd = listen_fd;
    for (ogni client c connesso) {           // ...e se i client esistenti hanno scritto
        FD_SET(c.fd, &readfds);
        if (c.fd > maxfd) maxfd = c.fd;
    }

    struct timeval tv = { .tv_sec = T, .tv_usec = 0 };   // svegliami al più tardi tra T sec

    int pronti = select(maxfd + 1, &readfds, NULL, NULL, &tv);

    if (pronti == 0) {                       // --- TIMEOUT: nessuno ha parlato per T sec ---
        invia_mappa_globale_a_tutti();       // broadcast periodico
        if (tempo_scaduto()) termina_partita();
        continue;
    }

    if (FD_ISSET(listen_fd, &readfds))       // --- NUOVA CONNESSIONE ---
        accetta_nuovo_client();              // accept(), aggiungi alla lista

    for (ogni client c connesso) {           // --- COMANDI DAI CLIENT ESISTENTI ---
        if (FD_ISSET(c.fd, &readfds))
            gestisci_dati_dal_client(&c);    // read non bloccante, bufferizza, esegui righe
    }
}
```

> Nota: `select()` "consuma" l'insieme `readfds` (lo modifica per dirti chi è pronto). Per
> questo va **ricostruito a ogni giro** del while.

### Socket non bloccanti
Siccome non possiamo permetterci di restare incastrati, mettiamo i socket in modalità
**non bloccante** con `fcntl(fd, F_SETFL, O_NONBLOCK)`. Così una `read()` su un socket senza
dati non aspetta: ritorna subito con `-1` ed `errno == EWOULDBLOCK`. Combinato con `select()`
(che ci dice di leggere solo quando c'è roba), questo rende il server reattivo e mai bloccato.

---

## 4. Avvio del server

Il server si lancia con `./server <porta>`. I passi per "aprire l'orecchio" alla rete:

```
socket()  → crea il fd del socket (IPv4, tipo TCP)
bind()    → lega il socket a una porta locale (es. 8080), così i client sanno dove bussare
listen()  → mette il socket in "ascolto": il SO inizia ad accodare le richieste di connessione
fcntl()   → rende il listen socket non bloccante
                                                                  
poi entra nel loop select() della sezione 3.
```

Promemoria sui vincoli della traccia: il server **non stampa su stdout**, **non legge da
stdin**, e usa **stderr solo se deve terminare per un errore fatale**. Quindi niente
`printf` di debug nella versione finale.

---

## 5. Ciclo di vita di una connessione

```
        client                       server
          │   connect() ───────────►  accept()  → nuovo fd, aggiunto alla lista client
          │                            (stato: CONNESSO, non ancora loggato)
          │
          │   REGISTER mario pwd ───►  controlla file utenti; se nick libero → salva → OK
          │   ◄─────────────────────  OK / ERR "nick già esistente"
          │
          │   LOGIN mario pwd ──────►  verifica credenziali su file; se ok e non già online
          │                            → assegna posizione casuale → stato: IN GIOCO
          │   ◄─────────────────────  OK + prima mappa locale
          │
          │   ... gioca (MOVE/WHO) ...
          │
          │   QUIT  /  crash ───────►  rileva chiusura (read ritorna 0) → libera lo slot
          │                            → chiude il fd → toglie il giocatore dalla lista
```

### Persistenza degli utenti su file
Le credenziali vanno **salvate su disco** (es. `users.dat`) così sopravvivono al riavvio del
server. Alla registrazione si **appende** una nuova voce; al login si **cerca** la voce e si
confronta la password.

```
REGISTER:  apro users.dat, controllo che il nick non esista già,
           scrivo (nick, password) in fondo, chiudo.
LOGIN:     apro users.dat, cerco il nick, confronto la password.
           se ok e nessuno è già online con quel nick → entra in gioco.
```

> Per un progetto didattico va bene anche salvare la password in chiaro; volendo si può
> conservare un semplice hash. La cosa importante è che la registrazione **persista**.

### Disconnessione
Quando un client chiude (volontariamente con `QUIT`, o di colpo), la sua `read()` sul server
ritorna **0** (oppure errore). Il server allora: rimuove il giocatore dalla lista, chiude il
fd, e libera la sua posizione. **Le celle già conquiste restano sue** sulla mappa globale
(la proprietà è pubblica e persistente fino a un eventuale ribaltamento).

---

## 6. Una mossa, dall'inizio alla fine

Questa è la sequenza più importante. Seguiamo un singolo `MOVE` dal tasto premuto al disegno
finale.

```
(1) UTENTE              preme il tasto "freccia su" (o 'w')
        │
(2) CLIENT              traduce in una riga di protocollo e la invia:
        │                   write(sock, "MOVE U\n", 7);
        ▼
(3) SERVER (select)     select() segnala che il fd di questo client è leggibile
        │
(4) SERVER (buffer)     legge i byte, li accumula, trova il '\n' → estrae "MOVE U"
        │
(5) SERVER (validazione)  calcola la cella di destinazione (x, y-1).
        │                 - fuori dai bordi?            → ERR, mossa annullata
        │                 - è un MURO?                  → rivela il muro al giocatore, ERR
        │                 - è LIBERA?                   → mossa valida, vai a (6)
        ▼
(6) SERVER (conquista)  sposta il giocatore: posizione = (x, y-1).
        │                 proprieta[x][y-1] = id_giocatore   ← CONQUISTA / RIBALTAMENTO
        │                 ricalcola i punteggi (chi possiede quante celle)
        ▼
(7) SERVER (fog-of-war) guarda la finestra (2r+1)x(2r+1) attorno alla nuova posizione e
        │                 aggiunge all'insieme "muri scoperti" di QUESTO giocatore i muri
        │                 che ricadono nella finestra (sezione 7).
        ▼
(8) SERVER (mappa locale) costruisce la mappa locale: per ogni cella della finestra mette
        │                 il simbolo giusto (muro scoperto / cella libera / proprietario /
        │                 posizione del giocatore) e la invia:  write(sock, "LOCAL ...\n");
        ▼
(9) CLIENT (rendering)  riceve "LOCAL ...", la interpreta e la **disegna** a schermo.
```

### Esempio numerico su una mini-griglia 5×5
Legenda: `.` cella libera mai conquistata · `#` muro · `A`/`B` cella posseduta da quel
giocatore · `@` posizione del giocatore che guarda.

Mario (`A`) è in (2,2). Stato globale reale (che solo il server conosce per intero):

```
   x→ 0 1 2 3 4
y0     . . # . .
y1     . . . . #
y2     . # @ . .        @ = Mario in (2,2)
y3     . . . # .
y4     # . . . .
```

Mario manda `MOVE R` (destra → cella (3,2)). È libera: si sposta, e (3,2) diventa sua (`A`).
Con raggio fog `r=1` (finestra 3×3 per l'esempio), la **mappa locale** che riceve Mario,
centrata sulla nuova posizione (3,2), contiene solo ciò che lui può vedere:

```
   . . .          ← celle (2,1)(3,1)(4,1): tutte libere
   A @ .          ← (2,2)=sua, (3,2)=@ lui ora, (4,2) libera
   . # .          ← (2,3) libera, (3,3)=# MURO appena scoperto, (4,3) libera
```

Mario ora **sa** che c'è un muro in (3,3): è stato "scoperto" ed entra nei suoi muri privati.
Un altro giocatore, finché non passa di lì, **non** vedrà quel muro.

---

## 7. Il fog-of-war

"Fog of war" = nebbia di guerra: vedi solo la zona che hai esplorato.

- Il server conosce **tutta** la mappa dei muri (è la verità).
- Ogni giocatore ha però un suo insieme privato: **"i muri che IO ho scoperto"**.
- A ogni mossa, il server guarda la finestra (2r+1)×(2r+1) attorno al giocatore e aggiunge a
  quell'insieme i muri che cadono nella finestra.
- Nella mappa locale che invia, mostra un muro **solo se** quel giocatore l'ha già scoperto.

Implementazione tipica dell'insieme "muri scoperti": una matrice di bit/booleani `H×W` per
ogni giocatore, `scoperto[x][y] = 1` quando la cella entra nella finestra. Occupa poco e si
consulta in tempo costante.

Differenza chiave da ricordare:

| Informazione        | Visibilità                                  |
|---------------------|---------------------------------------------|
| Muri / ostacoli     | **Privati**: ognuno vede solo ciò che esplora |
| Proprietà delle celle | **Pubbliche**: tutti vedono la mappa globale  |

---

## 8. Il broadcast periodico

Oltre alle mappe locali (che arrivano dopo ogni mossa), ogni `T` secondi il server manda a
**tutti** i client connessi:

- la **mappa globale di proprietà** (chi possiede ogni cella), e
- le **posizioni correnti** di tutti i giocatori.

Come si attiva senza un thread/timer separato? Grazie al **timeout di `select()`**: se per
`T` secondi nessuno ha mandato comandi, `select()` ritorna `0`, ed è lì che facciamo il
broadcast (ramo `pronti == 0` nel loop della sezione 3).

```
                 select() con timeout = T
                ┌───────────────────────┐
                │  qualcuno ha parlato?  │
                └─────────┬─────────────┘
                  sì      │      no (timeout)
            ┌─────────────┘             └──────────────┐
   servi i client/accetta          invia GLOBAL a tutti i client
   connessioni                     + controlla fine partita
```

> Dettaglio realistico: il broadcast non deve scattare *solo* quando c'è silenzio totale.
> Si tiene un timestamp dell'ultimo broadcast e, ogni volta che `select()` ritorna (anche per
> attività), si controlla se è passato almeno `T`; se sì, si manda. Così l'invio resta
> regolare anche con molto traffico. Il timeout di `select()` va calcolato come "quanto manca
> al prossimo broadcast".

---

## 9. Fine partita

C'è un **timeout globale** (es. 180s) per l'intera partita. Lo si controlla nel loop (di
nuovo, sfruttando i ritorni di `select()` e un timestamp d'inizio). Allo scadere:

1. Il server conta, per ogni giocatore, **quante celle possiede** sulla mappa globale.
2. Compila la **classifica** (vince chi ne ha di più).
3. Invia a tutti un messaggio `GAMEOVER` con la classifica.
4. Chiude le connessioni / azzera lo stato.

```
conteggio:  per ogni cella (x,y): se proprieta[x][y] == id → punteggio[id]++
classifica: ordina i giocatori per punteggio decrescente
vincitore:  il primo della classifica
```

---

## 10. Lato client

Il client si lancia con `./client <host> <porta>` e si connette con `connect()`.

Il client ha **due fonti di eventi contemporanee**:

- la **tastiera** (`stdin`): l'utente digita comandi / preme tasti per muoversi;
- il **socket**: il server manda mappe locali, broadcast globali, risposte.

Se il client aspettasse bloccato sulla tastiera, non vedrebbe arrivare i broadcast; se
aspettasse bloccato sul socket, non leggerebbe la tastiera. Soluzione: **stesso trucco del
server**, un `select()` su **entrambi** i fd (`stdin` e il socket):

```c
while (connesso) {
    fd_set rfds; FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);     // tastiera
    FD_SET(sock, &rfds);             // server
    select(maxfd+1, &rfds, NULL, NULL, NULL);

    if (FD_ISSET(STDIN_FILENO, &rfds)) leggi_comando_utente_e_invia();
    if (FD_ISSET(sock, &rfds))         leggi_dal_server_e_disegna();
}
```

### Rendering ASCII
Quando arriva una mappa (`LOCAL` o `GLOBAL`), il client la interpreta e la stampa a terminale
con simboli leggibili, ad esempio:

```
#  muro (scoperto da me)
.  cella libera mai conquistata
A  cella posseduta dal giocatore A      (lettere/colori diversi per giocatore)
@  la mia posizione
```

Il client **non decide nulla** del gioco: disegna ciò che riceve e inoltra ciò che l'utente
chiede. Comandi tipici offerti all'utente: muoversi (frecce/WASD → `MOVE`), `WHO` (lista
utenti), vedere la mappa, disconnettersi (`QUIT`).

---

## 11. Le strutture dati

Le strutture principali sul **server** (in normali variabili del processo, niente memoria
condivisa):

```c
// Parametri (in un header comune)
#define H 30           // righe della mappa
#define W 30           // colonne
#define R_FOG 3        // raggio finestra fog-of-war (finestra 7x7)
#define T_BROADCAST 5  // secondi tra un broadcast e l'altro
#define T_PARTITA 180  // durata partita in secondi

// La mappa vera (la conosce solo il server)
char  muri[H][W];          // 1 = muro, 0 = libera
int   proprieta[H][W];     // id del proprietario, -1 = nessuno

// Un giocatore connesso
typedef struct {
    int   fd;                  // socket verso questo client
    char  nick[32];
    int   x, y;                // posizione corrente
    int   stato;               // CONNESSO / LOGGATO / IN_GIOCO
    char  scoperto[H][W];      // fog-of-war privato: 1 = muro/cella già vista
    int   punteggio;           // n. celle possedute
    char  inbuf[BUFSZ];        // buffer per il framing (letture parziali)
    int   inlen;               // byte attualmente nel buffer
} Giocatore;

Giocatore giocatori[MAX_CLIENT];   // o una lista dinamica
int       n_giocatori;
```

Come si collegano:

```
listen_fd ──accept()──► nuovo Giocatore (fd, inbuf)
                              │ LOGIN
                              ▼
                       posizione (x,y) su  muri[][] (deve essere libera)
                              │ MOVE
                              ▼
                       aggiorna proprieta[x][y]  ← mappa globale pubblica
                       aggiorna scoperto[][]      ← fog-of-war privato
                              │
                              ▼
                       costruisci LOCAL (finestra attorno a x,y) → invia su fd
                       ogni T sec: costruisci GLOBAL (tutta proprieta[][] + posizioni) → a tutti
```

---

## 12. Glossario

- **fd (file descriptor)**: numero intero che identifica un file/socket aperto. Su un socket
  si fa `read`/`write` come su un file.
- **socket TCP**: canale di comunicazione affidabile e ordinato tra due programmi in rete.
- **bind / listen / accept**: i tre passi con cui un server "apre l'orecchio" su una porta e
  accetta connessioni in arrivo.
- **`select()`**: system call che aspetta su **più** fd insieme e ti dice quali sono pronti,
  con un timeout opzionale. Cuore del server (e del client) non bloccante.
- **`fd_set` / `FD_SET` / `FD_ISSET`**: l'insieme di fd che passi a `select()` e le macro per
  riempirlo / interrogarlo.
- **socket non bloccante (`O_NONBLOCK`)**: una `read`/`write` non resta in attesa; se non può
  procedere ritorna subito con `EWOULDBLOCK`.
- **framing**: regola per capire dove finisce un messaggio dentro un flusso di byte. Qui:
  ogni messaggio termina con `\n`.
- **lettura parziale**: quando una `read()` restituisce solo una parte di un messaggio (o più
  messaggi insieme); si gestisce con un **buffer di input**.
- **fog-of-war**: meccanica per cui ogni giocatore vede solo la parte di mappa (gli ostacoli)
  che ha esplorato.
- **conquista / ribaltamento**: una cella appartiene all'ultimo che ci è passato; se ci passa
  un altro, cambia padrone.
- **broadcast**: invio dello stesso messaggio a tutti i client (qui: la mappa globale ogni
  `T` secondi).
- **race condition**: errore che nasce quando due esecuzioni toccano lo stesso dato insieme.
  Con il modello `select()` a singolo processo **non può accadere**, perché c'è un solo
  flusso di esecuzione.
