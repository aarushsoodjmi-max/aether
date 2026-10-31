#include "version.h"
#include "util.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int order(int c) { if (isdigit(c)) return 0; if (isalpha(c)) return c; if (c == '~') return -1; if (c) return c + 256; return 0; }
static int verrevcmp(const char *a, const char *b)
{
    while (*a || *b) {
        int first = 0;
        while ((*a && !isdigit((unsigned char)*a)) || (*b && !isdigit((unsigned char)*b))) {
            int ac = order((unsigned char)*a), bc = order((unsigned char)*b);
            if (ac != bc) return ac - bc;
            a++; b++;
        }
        while (*a == '0') a++;
        while (*b == '0') b++;
        while (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) { if (!first) first = *a - *b; a++; b++; }
        if (isdigit((unsigned char)*a)) return 1;
        if (isdigit((unsigned char)*b)) return -1;
        if (first) return first;
    }
    return 0;
}
static void split(const char *v, unsigned long *epoch, char **up, char **rev)
{
    char *s = xstrdup(v); *epoch = 0;
    char *c = strchr(s, ':');
    char *rest = s;
    if (c) { *c = 0; *epoch = strtoul(s, NULL, 10); rest = c + 1; }
    char *h = strrchr(rest, '-');
    if (h) { *h = 0; *rev = xstrdup(h + 1); } else *rev = xstrdup("");
    *up = xstrdup(rest); free(s);
}
int ver_cmp(const char *a, const char *b)
{
    unsigned long ea, eb; char *ua, *ra, *ub, *rb;
    split(a, &ea, &ua, &ra); split(b, &eb, &ub, &rb);
    int r = ea < eb ? -1 : ea > eb ? 1 : 0;
    if (!r) r = verrevcmp(ua, ub);
    if (!r) r = verrevcmp(ra, rb);
    free(ua); free(ra); free(ub); free(rb);
    return r < 0 ? -1 : r > 0 ? 1 : 0;
}

const char *op_str(VerOp op)
{
    switch (op) { case OP_LT: return "<<"; case OP_LE: return "<="; case OP_EQ: return "="; case OP_GE: return ">="; case OP_GT: return ">>"; default: return ""; }
}
int atom_parse(char **tok, int ntok, Atom *out, char *err)
{
    memset(out, 0, sizeof *out);
    if (ntok != 1 && ntok != 3) { set_err(err, "dependency must be 'name' or 'name OP version'"); return -1; }
    if (!valid_pkg_name(tok[0])) { set_err(err, "bad dependency name '%s'", tok[0]); return -1; }
    out->name = xstrdup(tok[0]);
    if (ntok == 1) return 0;
    static const struct { const char *s; VerOp op; } ops[] = { {"<<", OP_LT}, {"<=", OP_LE}, {"=", OP_EQ}, {">=", OP_GE}, {">>", OP_GT} };
    out->op = OP_ANY;
    for (size_t i = 0; i < sizeof ops / sizeof ops[0]; i++) if (!strcmp(tok[1], ops[i].s)) out->op = ops[i].op;
    if (out->op == OP_ANY) { set_err(err, "bad version operator '%s' (use << <= = >= >>)", tok[1]); free(out->name); out->name = NULL; return -1; }
    if (!valid_version(tok[2])) { set_err(err, "bad version '%s'", tok[2]); free(out->name); out->name = NULL; return -1; }
    out->ver = xstrdup(tok[2]); return 0;
}
int atom_satisfied_by(const Atom *a, const char *version)
{
    if (a->op == OP_ANY) return 1;
    if (!version) return 0;
    int c = ver_cmp(version, a->ver);
    switch (a->op) { case OP_LT: return c < 0; case OP_LE: return c <= 0; case OP_EQ: return c == 0; case OP_GE: return c >= 0; case OP_GT: return c > 0; default: return 1; }
}
char *atom_str(const Atom *a) { return a->op == OP_ANY ? xstrdup(a->name) : xsprintf("%s %s %s", a->name, op_str(a->op), a->ver); }
void atom_free(Atom *a) { free(a->name); free(a->ver); a->name = a->ver = NULL; }
