#ifndef USERS_H
#define USERS_H

/*
 * users.h — persistenza degli account su file (users.dat).
 *
 * Formato del file: una riga per utente, "nick pass\n".
 * LIMITE NOTO (didattico): la password è memorizzata IN CHIARO. In un sistema
 * reale andrebbe salvato almeno un hash salato; qui si privilegia la
 * semplicità e la leggibilità del file.
 */

/* Ritorna 1 se il nickname esiste già nel file, 0 altrimenti. */
int utenti_esiste(const char *nick);

/* Registra un nuovo utente appendendolo al file. Ritorna:
 *    0 = registrato con successo;
 *   -1 = nickname già esistente;
 *   -2 = input non valido o errore di I/O. */
int utenti_registra(const char *nick, const char *pass);

/* Verifica le credenziali: 1 se nick+pass corrispondono a una voce, 0 altrimenti. */
int utenti_verifica(const char *nick, const char *pass);

#endif /* USERS_H */
