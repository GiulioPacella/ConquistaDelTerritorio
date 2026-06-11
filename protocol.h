#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h — definizione del protocollo applicativo.
 *
 * Il protocollo è TESTUALE e LINEA-ORIENTATO: ogni messaggio (in entrambe le
 * direzioni) è una riga di testo terminata da '\n'. Questo lo rende semplice da
 * implementare, da debuggare a occhio e da testare a mano con strumenti come
 * `nc` (netcat).
 *
 * Poiché TCP è un flusso di byte (le read possono spezzare o aggregare i
 * messaggi), sia client che server accumulano i byte in un buffer ed estraggono
 * UNA riga completa per volta (framing tramite '\n'). Vedi net_util.h.
 *
 * ------------------------------------------------------------------------
 * Comandi Client -> Server
 * ------------------------------------------------------------------------
 *   REGISTER <nick> <pass>   registra un nuovo account (nick deve essere libero)
 *   LOGIN    <nick> <pass>   autentica ed entra in gioco
 *   MOVE     <U|D|L|R>       muove di una cella (Up/Down/Left/Right)
 *   WHO                      richiede l'elenco dei giocatori loggati (USERS)
 *   MAP                      richiede subito una mappa globale (GLOBAL)
 *   QUIT                     chiude la sessione
 *
 * ------------------------------------------------------------------------
 * Risposte Server -> Client
 * ------------------------------------------------------------------------
 *   OK [msg]                 esito positivo (con messaggio opzionale)
 *   ERR <msg>                esito negativo, con spiegazione
 *   LOCAL ...                mappa locale (fog-of-war) — multi-riga, chiusa da END
 *   GLOBAL ...               mappa globale delle proprietà — multi-riga, chiusa da END
 *   USERS ...                elenco giocatori — multi-riga, chiuso da END
 *   GAMEOVER ...             classifica di fine partita — multi-riga, chiusa da END
 *
 * ------------------------------------------------------------------------
 * Serializzazione (da rispettare ESATTAMENTE in client e server)
 * ------------------------------------------------------------------------
 *  LOCAL:
 *      LOCAL <cx> <cy> <r>\n            (centro = posizione del giocatore)
 *      <2r+1 righe da 2r+1 caratteri>   (finestra centrata su (cx,cy))
 *      END\n
 *
 *  GLOBAL:
 *      GLOBAL <W> <H> <nplayers>\n
 *      <H righe da W caratteri>         (mappa proprietà, senza il simbolo '@')
 *      P <id> <nick> <x> <y> <score>\n  (ripetuta nplayers volte)
 *      END\n
 *
 *  USERS:
 *      USERS <n>\n
 *      <id> <nick>\n                    (ripetuta n volte)
 *      END\n
 *
 *  GAMEOVER:
 *      GAMEOVER <n>\n
 *      <pos> <nick> <score>\n           (classifica decrescente, n righe)
 *      END\n
 */

/* ----- Comandi Client -> Server ----- */
#define CMD_REGISTER "REGISTER"
#define CMD_LOGIN    "LOGIN"
#define CMD_MOVE     "MOVE"
#define CMD_WHO      "WHO"
#define CMD_MAP      "MAP"
#define CMD_QUIT     "QUIT"

/* ----- Risposte Server -> Client ----- */
#define REP_OK       "OK"
#define REP_ERR      "ERR"
#define REP_LOCAL    "LOCAL"
#define REP_GLOBAL   "GLOBAL"
#define REP_USERS    "USERS"
#define REP_GAMEOVER "GAMEOVER"
#define REP_END      "END"     /* marcatore di fine dei messaggi multi-riga */

/* ----- Simboli usati nelle mappe (LOCAL e GLOBAL) ----- */
#define SIM_MURO     '#'   /* muro scoperto                         */
#define SIM_LIBERA   '.'   /* cella libera senza proprietario       */
#define SIM_IO       '@'   /* posizione del giocatore (solo LOCAL)  */
#define SIM_IGNOTO   '?'   /* fuori mappa / non ancora noto         */
/* Le celle possedute usano un carattere singolo ricavato dall'id del
   proprietario tramite simbolo_proprietario() (vedi sotto). */

/*
 * Mappa l'id di un giocatore (>= 0) su un singolo carattere stampabile:
 *   0..9   -> '0'..'9'
 *   10..35 -> 'a'..'z'
 *   oltre  -> '+'
 * Per id < 0 (nessun proprietario) ritorna il simbolo di cella libera.
 *
 * È static inline così può stare nell'header condiviso senza violazioni ODR e
 * senza warning di funzione inutilizzata.
 */
static inline char simbolo_proprietario(int id) {
    if (id < 0)  return SIM_LIBERA;
    if (id <= 9) return (char)('0' + id);
    if (id <= 35) return (char)('a' + (id - 10));
    return '+';
}

#endif /* PROTOCOL_H */
