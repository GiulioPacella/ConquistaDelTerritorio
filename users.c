#include "common.h"     /* PRIMO: feature-test macro */
#include "users.h"

#include <stdio.h>
#include <string.h>

#define FILE_UTENTI "users.dat"

/* I buffer di lettura sono dimensionati MAX_NICK+1 / MAX_PASS+1 (= 32). I campi
   "%31s" nelle fscanf limitano la lettura a 31 caratteri + terminatore, coerenti
   con MAX_NICK = MAX_PASS = 31. */

int utenti_esiste(const char *nick) {
    FILE *f = fopen(FILE_UTENTI, "r");
    if (f == NULL) return 0;   /* file inesistente: nessun utente registrato */

    char r_nick[MAX_NICK + 1];
    char r_pass[MAX_PASS + 1];
    int trovato = 0;
    while (fscanf(f, "%31s %31s", r_nick, r_pass) == 2) {
        if (strcmp(r_nick, nick) == 0) {
            trovato = 1;
            break;
        }
    }
    fclose(f);
    return trovato;
}

int utenti_registra(const char *nick, const char *pass) {
    /* validazioni di base: campi non vuoti ed entro i limiti */
    if (nick[0] == '\0' || pass[0] == '\0') return -2;
    if (strlen(nick) > MAX_NICK || strlen(pass) > MAX_PASS) return -2;

    if (utenti_esiste(nick)) return -1;

    FILE *f = fopen(FILE_UTENTI, "a");
    if (f == NULL) return -2;
    fprintf(f, "%s %s\n", nick, pass);
    fclose(f);
    return 0;
}

int utenti_verifica(const char *nick, const char *pass) {
    FILE *f = fopen(FILE_UTENTI, "r");
    if (f == NULL) return 0;

    char r_nick[MAX_NICK + 1];
    char r_pass[MAX_PASS + 1];
    int ok = 0;
    while (fscanf(f, "%31s %31s", r_nick, r_pass) == 2) {
        if (strcmp(r_nick, nick) == 0) {
            ok = (strcmp(r_pass, pass) == 0);
            break;
        }
    }
    fclose(f);
    return ok;
}
