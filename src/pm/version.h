/* Version ordering and dependency constraint atoms.
 * Ordering is the well-known dpkg algorithm (epoch:upstream-revision, '~' sorts lowest). */
#ifndef AETHER_PM_VERSION_H
#define AETHER_PM_VERSION_H
int ver_cmp(const char *a, const char *b);   /* <0, 0, >0 */

typedef enum { OP_ANY = 0, OP_LT, OP_LE, OP_EQ, OP_GE, OP_GT } VerOp;
typedef struct { char *name; VerOp op; char *ver; } Atom;   /* "name" or "name >= 1.2" */

/* Parse from whitespace-split tokens: tok[0]=name, optionally tok[1]=op tok[2]=version. */
int atom_parse(char **tok, int ntok, Atom *out, char *err);
int atom_satisfied_by(const Atom *a, const char *version);  /* 1 if 'version' meets the constraint */
char *atom_str(const Atom *a);                              /* malloc'd "name op ver" */
void atom_free(Atom *a);
const char *op_str(VerOp op);
#endif
