#ifndef PROTOCOL_H
#define PROTOCOL_H

// protocol.h — definizione del protocollo applicativo.
  
// Comandi Client -> Server 
#define CMD_REGISTER "REGISTER"
#define CMD_LOGIN    "LOGIN"
#define CMD_MOVE     "MOVE"
#define CMD_WHO      "WHO"
#define CMD_QUIT     "QUIT"

// Risposte Server -> Client 
#define REP_OK       "OK"
#define REP_ERR      "ERR"
#define REP_LOCAL    "LOCAL"
#define REP_GLOBAL   "GLOBAL"
#define REP_USERS    "USERS"
#define REP_GAMEOVER "GAMEOVER"
#define REP_END      "END"     /* marcatore di fine dei messaggi multi-riga */

// Simboli nelle mappe
#define SIM_MURO     '#'   /* muro scoperto                         */
#define SIM_LIBERA   '.'   /* cella libera senza proprietario       */
#define SIM_IO       '@'   /* posizione del giocatore (solo LOCAL)  */
#define SIM_IGNOTO   '?'   /* fuori mappa / non ancora noto         */
/* Le celle possedute usano un carattere singolo ricavato dall'id del
   proprietario tramite simbolo_proprietario() (vedi sotto). */

/*
 * Mappa l'id di un giocatore (>= 0) su un singolo carattere stampabile:
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
