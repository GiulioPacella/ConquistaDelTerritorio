# Conquista del territorio — client/server in C (socket TCP)

Gioco multigiocatore a conquista di celle su griglia, con muri a *fog-of-war*.
Architettura client/server: un solo server concorrente (un processo, `select()`,
socket non bloccanti — niente fork/thread) serve molti client connessi via TCP.

## Compilazione

```sh
make            # compila server e client
make server     # solo il server
make client     # solo il client
make clean      # rimuove eseguibili e file oggetto
```

Flag usati: `-Wall -Wextra -pedantic -std=c11`. La compilazione è **senza warning**.

## Avvio

```sh
./server <porta> [seed]      # es. ./server 8080
./client <host> <porta>      # es. ./client 127.0.0.1 8080
```

- `seed` (opzionale) rende riproducibile la generazione della mappa. Se omesso,
  ogni nuova partita usa un seme casuale basato sull'orologio.
- Il server **non** stampa su stdout e **non** legge da stdin; scrive su stderr
  solo per errori fatali di avvio.

## Comandi del client

Si possono digitare direttamente i comandi di protocollo, oppure usare le
scorciatoie di movimento `W A S D` (su / sinistra / giù / destra).

| Comando                   | Effetto                                  |
|---------------------------|------------------------------------------|
| `REGISTER <nick> <pass>`  | crea un nuovo account                    |
| `LOGIN <nick> <pass>`     | autentica ed entra in gioco              |
| `MOVE <U\|D\|L\|R>`       | muove di una cella                       |
| `W` `A` `S` `D`           | scorciatoie per `MOVE U/L/D/R`           |
| `WHO`                     | elenco dei giocatori loggati             |
| `MAP`                     | richiede subito la mappa globale         |
| `QUIT`                    | esce                                     |

## Regole in breve

- La mappa è una griglia `MAPPA_W`×`MAPPA_H` (default 30×30) con muri pseudo-casuali
  (~20%). Entrando in una cella libera la si **conquista**; se un altro vi passa la
  **ribalta**.
- I **muri sono privati** (fog-of-war): ognuno scopre solo la finestra 7×7 attorno a
  sé. Le **proprietà sono pubbliche**: tutti vedono la stessa mappa globale.
- Ogni `T_BROADCAST` secondi (default 5) il server invia a tutti la mappa globale.
- La partita parte al **primo login** e dura `T_PARTITA` secondi (default 180). Allo
  scadere arriva `GAMEOVER` con la classifica; lo stato viene azzerato e una **nuova
  partita riparte** al primo `MOVE`/`LOGIN` successivo, **senza riavviare** i client.

## Parametri configurabili

In `common.h`: `MAPPA_H`, `MAPPA_W`, `R_FOG`, `T_BROADCAST`, `T_PARTITA`,
`DENSITA_MURI`, `MAX_LINE`, `MAX_NICK`, `MAX_CLIENT`.

## File del progetto

| File                  | Contenuto                                                    |
|-----------------------|--------------------------------------------------------------|
| `common.h`            | parametri, limiti, feature-test macro, tipi (stati)          |
| `protocol.h`          | definizione e documentazione del protocollo                  |
| `net_util.h/.c`       | socket, buffer di input (framing), coda di output            |
| `users.h/.c`          | persistenza account su `users.dat`                           |
| `game.h/.c`           | stato e logica di gioco, serializzazione dei messaggi        |
| `server.c`            | server `select()`: loop, accept, dispatch, broadcast, segnali|
| `client.c`            | client interattivo non bloccante, rendering ASCII/colori     |
| `Makefile`            | target `server`, `client`, `clean`                           |
| `RELAZIONE.md`        | relazione tecnica dettagliata                                |
| `come_funziona.md`    | guida didattica introduttiva                                 |

## Note

- Le password sono salvate **in chiaro** in `users.dat` (semplificazione didattica,
  vedi commenti in `users.c`).
- Il server usa `select()`: gli fd devono restare sotto `FD_SETSIZE` (1024). Per
  onorare il "nessun limite a priori" della traccia, `MAX_CLIENT` è portato vicino
  a quel tetto (`1000`); oltre `FD_SETSIZE` le connessioni sono rifiutate.
- Verifica memoria: `valgrind --leak-check=full ./server <porta>`. Il codice è scritto
  per essere *valgrind-clean* (l'unica allocazione dinamica è la coda di output, sempre
  liberata alla disconnessione e allo shutdown).
