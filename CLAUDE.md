# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Lingua

Codice, commenti, documentazione e messaggi di protocollo sono **in italiano**. Mantieni questa convenzione.

## Build & run

```sh
make            # compila server e client
make server     # solo il server
make client     # solo il client
make clean      # rimuove eseguibili e *.o

./server <porta> [seed]      # seed opzionale = mappa riproducibile
./client <host> <porta>

valgrind --leak-check=full ./server <porta>   # verifica memoria (deve restare clean)
```

Non esiste una suite di test automatica. Si verifica a mano con `nc` o con un driver
socket (es. Python). Esempio rapido di sessione:

```sh
nc 127.0.0.1 8080
REGISTER mario segreto
LOGIN mario segreto
MOVE R
WHO
QUIT
```

Per testare il rendering del client si può pilotarlo via pipe (stdout non-tty →
niente escape di pulizia schermo, le mappe restano in sequenza ispezionabile):

```sh
./server 8099 42 &      # seed fisso = mappa riproducibile
{ printf 'REGISTER t t\nLOGIN t t\n'; sleep 0.3; printf 'd\ns\n'; sleep 0.3; printf 'QUIT\n'; } \
  | ./client 127.0.0.1 8099 > /tmp/out.txt
```

**Attenzione:** i `REGISTER` di test sporcano `users.dat` (l'account reale è
`giulio 123`): rimuovere le righe di test a fine verifica.

## Vincoli inderogabili (non violarli)

- Deve compilare con `-Wall -Wextra -pedantic -std=c11` **senza alcun warning** (i flag sono già nel Makefile).
- **Solo system call UNIX + libc**. È ammesso `select()`; **vietati** `poll`/`epoll`/`thread`/`fork`/memoria condivisa/IPC. La concorrenza è un singolo processo con I/O multiplexing.
- `common.h` definisce le **feature-test macro** (`_POSIX_C_SOURCE`, `_DEFAULT_SOURCE`) e **deve essere il primo `#include` di ogni `.c`**, prima di qualunque header di sistema. Spostarlo o ometterlo reintroduce warning di dichiarazione implicita.
- Il **server non scrive su stdout e non legge da stdin**; usa `stderr` solo per errori fatali di avvio.
- Mantieni il codice **valgrind-clean**: l'unica memoria dinamica è il buffer di `CodaOut`, liberato in `rimuovi_client` e `chiudi_tutto`. Ogni nuova allocazione va liberata su disconnessione e shutdown.
- **Tetto ai client (`MAX_CLIENT` = 1000 in `common.h`)**: la traccia chiede "nessun limite a priori", ma `select()` impone che ogni fd resti `< FD_SETSIZE` (tipicamente 1024) — `FD_SET` oltre quel valore è UB. Il limite non è arbitrario ma imposto dal multiplexing; è tenuto sotto il tetto lasciando margine per gli fd riservati. A slot esauriti, `accetta_connessioni` (in `server.c`) risponde `ERR server pieno` e chiude.

## Architettura

Server autorevole: tutta la logica di gioco è sul server; un singolo flusso di
esecuzione elimina le race condition senza lock. Il client inoltra comandi e non
decide nulla, ma mantiene un **modello locale persistente** di ciò che ha visto
(muri scoperti, proprietà, posizioni) per il rendering.

**Documentazione:** `RELAZIONE.md` è la relazione tecnica di riferimento (architettura, protocollo, loop, casi limite); `come_funziona.md` è una spiegazione discorsiva più didattica. `README.md` è la guida utente — nota che alcuni suoi valori sono disallineati (cita mappa 30×30 e finestra 7×7): fa fede `common.h` (20×20, torcia 5×5).

**Moduli** (vedi `RELAZIONE.md` per il dettaglio):
- `common.h` — parametri (`MAPPA_H/W` 20x20, `R_FOG` 2 → torcia 5x5, `T_BROADCAST`, `T_FLASH` durata "luce globale", `T_PARTITA`, `DENSITA_MURI`, limiti), enum `StatoClient`/`StatoPartita`.
- `protocol.h` — costanti del protocollo e `simbolo_proprietario(id)` (id→carattere mappa).
- `net_util.[ch]` — `BufferIn` (framing a righe `\n`, gestisce letture parziali/aggregate, cap `MAX_LINE`), `CodaOut` (coda output dinamica per write parziali/`EAGAIN`), creazione listening socket / connect, non-blocking.
- `users.[ch]` — account su `users.dat` (`nick pass` per riga, **password in chiaro**, limite didattico noto).
- `game.[ch]` — `Mappa` (muri privati + proprietà pubbliche), `Giocatore` (slot), mosse, fog-of-war, punteggi, serializzazione `invia_local/global/users/gameover`.
- `server.c` — loop `select()`, accept, dispatch comandi, broadcast, segnali, shutdown. Al `LOGIN` invia sia `LOCAL` (torcia allo spawn) sia `GLOBAL`.
- `client.c` — `select()` su stdin+socket (con timeout quando la "luce globale" è accesa), modello persistente della mappa, rendering ASCII + colori ANSI opzionali.

### Concetti chiave da capire prima di modificare

- **Loop `select()`** (`server.c` `main`): `readfds` = listening + tutti i client; `writefds` = **solo** i client con coda non vuota. Il timeout è `min(tempo al prossimo broadcast, tempo a fine partita)`; i compiti periodici (broadcast/gameover) sono guidati da timestamp, non solo dal ramo `select()==0`. I set vanno **ricostruiti a ogni giro**.
- **Framing**: TCP è un flusso di byte. Mai assumere "un `read()` = un messaggio". Si accumula in `BufferIn` e si estrae **una riga per volta** con `buf_in_estrai_riga`. Vale anche nel client.
- **Output mai bloccante**: non chiamare `send()` sparso; accoda con `coda_accoda*` su `g->out` e lascia che il loop dreni via `writefds`. (Eccezione: il client usa `send` diretto perché il suo socket è bloccante e i comandi sono piccoli.)
- **Identità**: `id` = indice di slot in `giocatori[MAX_CLIENT]`, stabile, mappato a un carattere sulla mappa. Le celle conquistate persistono con quell'id anche dopo la disconnessione (limite noto: riuso slot eredita le celle; azzerate a fine partita).
- **Macchina a stati**: `ST_CONNESSO` ammette solo `REGISTER/LOGIN/QUIT`; `ST_IN_GIOCO` ammette `MOVE/WHO/QUIT`. Comando valido ma in stato errato → `ERR` esplicativo. Il dispatch è in `gestisci_riga`.
- **Ciclo di vita partita**: parte al primo `LOGIN` (`avvia_nuova_partita`); a `T_PARTITA` scaduto → `GAMEOVER` + `termina_partita` (azzeramento, stato `PARTITA_FERMA`); i client restano loggati e una nuova partita riparte al primo `MOVE`/`LOGIN` successivo, senza riavvio.
- **Fog-of-war (server)**: muri privati per giocatore (`scoperto[][]`), rivelati nella finestra `(2·R_FOG+1)²` a ogni mossa/spawn e accumulati; proprietà delle celle sempre pubbliche nel protocollo (il filtro di visibilità è nel rendering del client).
- **Rendering e visibilità (client, `client.c`)**: esiste UNA sola mappa disegnata; `LOCAL` (torcia) e `GLOBAL` (snapshot) si fondono sul modello `cli_prop`/`cli_muro`/`cli_altri`. Regole: i **muri scoperti** (`cli_muro`) si disegnano sempre e per sempre; proprietà e giocatori si disegnano **solo dentro la torcia 5x5**, tranne durante la **"luce globale"** (`T_FLASH` secondi dopo ogni `GLOBAL`, spenta dal timeout della `select`) in cui si vede tutto fuorché i muri non scoperti. La torcia è delimitata da una **cornice-corona**: le celle a distanza Chebyshev `R_FOG+1` sono sostituite da `+ - |`. Ogni redraw pulisce schermo e scrollback (`\033[H\033[2J\033[3J`, solo su tty). L'id proprio arriva da `OK login id=<n>`; a `GAMEOVER` il modello si azzera (`reset_modello`).

### Serializzazione del protocollo

`LOCAL`/`GLOBAL`/`USERS`/`GAMEOVER` sono multi-riga e terminano con `END`. Il formato è
fissato e va replicato **identico** tra `game.c` (produzione) e `client.c` (parsing):
modifiche al formato richiedono di aggiornare entrambi i lati. Dettaglio in `protocol.h`.
