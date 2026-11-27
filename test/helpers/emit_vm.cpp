// Differential-test helper: evaluates programs with the firmware's real VM (src/pattern/expr_vm.cpp).
//
// Reads lines from stdin, one program per line:
//   x t idx level bass mid treb b0 b1 b2 b3 b4 b5 b6 b7 nOps op arg op arg ...
// and prints one result per line (%.9g), or "INVALID <error>" if expr_validate rejects the program.
// test/js/test_differential.js compiles the same expressions with compiler.js and compares.
#include <stdio.h>
#include "pattern/expr_vm.cpp"

int main() {
    expr_init();
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        ExprCtx c{};
        float bands[8];
        double v[15];
        int n = 0, pos = 0;
        for (int i = 0; i < 15; i++) {
            if (sscanf(line + pos, "%lf%n", &v[i], &n) != 1) return 2;
            pos += n;
        }
        c.x = (float)v[0]; c.t = (float)v[1]; c.idx = (float)v[2]; c.level = (float)v[3];
        c.bass = (float)v[4]; c.mid = (float)v[5]; c.treb = (float)v[6];
        for (int b = 0; b < 8; b++) bands[b] = (float)v[7 + b];
        c.bands = bands;

        int nOps = 0;
        if (sscanf(line + pos, "%d%n", &nOps, &n) != 1) return 2;
        pos += n;
        ExprProgram p{};
        for (int i = 0; i < nOps; i++) {
            int op; double arg;
            if (sscanf(line + pos, "%d %lf%n", &op, &arg, &n) != 2) return 2;
            pos += n;
            if (p.count < 255) p.ops[p.count++] = ExprOpcode{(uint8_t)op, (float)arg};
        }
        const ExprError e = expr_validate(&p);
        if (e != EXPR_OK) printf("INVALID %s\n", expr_error_str(e));
        else              printf("%.9g\n", expr_eval(&p, &c));
    }
    return 0;
}
