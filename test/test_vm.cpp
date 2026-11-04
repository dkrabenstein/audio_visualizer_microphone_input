// Native harness: exercises expr_validate() and expr_eval() off-target,
#include <initializer_list>
// and emits results for differential comparison against compiler.js.
#include "../firmware/expr_vm.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int pass=0, fail=0;
static void ok(bool c, const char*m){ if(c) pass++; else {fail++; printf("FAIL: %s\n", m);} }

static ExprProgram mk(std::initializer_list<ExprOpcode> l){
    ExprProgram p{}; p.count=0;
    for(auto&o:l) p.ops[p.count++]=o;
    return p;
}

int main(int argc, char**argv){
    expr_init();

    float bands[8]={0.1f,0.2f,0.3f,0.4f,0.5f,0.6f,0.7f,0.8f};
    ExprCtx c{}; c.x=0.37f; c.t=2.5f; c.idx=1.0f; c.level=0.6f;
    c.bass=0.8f; c.mid=0.4f; c.treb=0.2f; c.bands=bands;

    // ---- validator: things that must be REJECTED ----
    { ExprProgram p=mk({}); ok(expr_validate(&p)==EXPR_ERR_EMPTY,"empty"); }
    { ExprProgram p=mk({{OP_ADD,0}}); ok(expr_validate(&p)==EXPR_ERR_UNDERFLOW,"underflow"); }
    { ExprProgram p=mk({{OP_CONST,1},{OP_CONST,2}});
      ok(expr_validate(&p)==EXPR_ERR_NOT_SINGLE_RESULT,"two results"); }
    { ExprProgram p=mk({{200,0}}); ok(expr_validate(&p)==EXPR_ERR_UNKNOWN_OP,"unknown op"); }
    { ExprProgram p=mk({{OP_BAND,8}}); ok(expr_validate(&p)==EXPR_ERR_BAD_ARG,"band 8"); }
    { ExprProgram p=mk({{OP_BAND,-1}}); ok(expr_validate(&p)==EXPR_ERR_BAD_ARG,"band -1"); }
    { ExprProgram p=mk({{OP_BAND,2.5f}}); ok(expr_validate(&p)==EXPR_ERR_BAD_ARG,"band frac"); }
    { ExprProgram p=mk({{OP_CONST,NAN}}); ok(expr_validate(&p)==EXPR_ERR_NAN_CONST,"nan const"); }
    { ExprProgram p{}; p.count=0;
      for(int i=0;i<EXPR_STACK_MAX+1;i++) p.ops[p.count++]={OP_CONST,1};
      ok(expr_validate(&p)==EXPR_ERR_OVERFLOW,"stack overflow"); }
    { ExprProgram p{}; p.count=EXPR_MAX_OPS+1;
      ok(expr_validate(&p)==EXPR_ERR_TOO_LONG,"too long"); }

    // ---- validator: must ACCEPT and record depth ----
    { ExprProgram p=mk({{OP_CONST,1},{OP_CONST,2},{OP_ADD,0}});
      ok(expr_validate(&p)==EXPR_OK,"1+2 valid");
      ok(p.maxDepth==2,"maxDepth 2"); }

    // ---- guarded arithmetic ----
    { ExprProgram p=mk({{OP_CONST,1},{OP_CONST,0},{OP_DIV,0}});
      expr_validate(&p); ok(expr_eval(&p,&c)==0.0f,"1/0 -> 0"); }
    { ExprProgram p=mk({{OP_CONST,5},{OP_CONST,0},{OP_MOD,0}});
      expr_validate(&p); ok(expr_eval(&p,&c)==0.0f,"5%0 -> 0"); }
    { ExprProgram p=mk({{OP_CONST,-4},{OP_SQRT,0}});
      expr_validate(&p); ok(expr_eval(&p,&c)==0.0f,"sqrt(-4) -> 0"); }
    { ExprProgram p=mk({{OP_CONST,-2},{OP_CONST,0.5f},{OP_POW,0}});
      expr_validate(&p); ok(expr_eval(&p,&c)==0.0f,"(-2)^0.5 -> 0"); }
    { ExprProgram p=mk({{OP_CONST,1e30f},{OP_CONST,1e30f},{OP_MUL,0}});
      expr_validate(&p); ok(isfinite(expr_eval(&p,&c)),"overflow folds finite"); }

    // ---- sine LUT accuracy vs libm ----
    float maxerr=0;
    for(int i=0;i<20000;i++){
        float a = -50.0f + 100.0f*(float)i/20000.0f;
        ExprProgram p=mk({{OP_CONST,a},{OP_SIN,0}});
        expr_validate(&p);
        float e = fabsf(expr_eval(&p,&c) - sinf(a));
        if(e>maxerr) maxerr=e;
    }
    printf("sine LUT max abs error over [-50,50] rad: %.3e (1/255 = %.3e)\n",
           maxerr, 1.0f/255.0f);
    ok(maxerr < 1e-4f, "LUT within 1e-4");
    ok(maxerr < 1.0f/255.0f/4.0f, "LUT error well under LED quantisation");

    // ---- emit values for differential test against JS ----
    if(argc>1 && strcmp(argv[1],"--emit")==0){
        struct { const char*name; ExprProgram p; } tv[] = {
          {"0.5+0.5*sin(tau*x+t)", mk({{OP_CONST,0.5f},{OP_CONST,0.5f},
              {OP_CONST,6.28318530718f},{OP_X,0},{OP_MUL,0},{OP_T,0},{OP_ADD,0},
              {OP_SIN,0},{OP_MUL,0},{OP_ADD,0}})},
          {"fract(x*8+t*0.5)", mk({{OP_X,0},{OP_CONST,8},{OP_MUL,0},
              {OP_T,0},{OP_CONST,0.5f},{OP_MUL,0},{OP_ADD,0},{OP_FRACT,0}})},
          {"band(0)*2+treb", mk({{OP_BAND,0},{OP_CONST,2},{OP_MUL,0},
              {OP_TREB,0},{OP_ADD,0}})},
          {"tri(x+t)", mk({{OP_X,0},{OP_T,0},{OP_ADD,0},{OP_TRI,0}})},
          {"sat(bass*3-1)", mk({{OP_BASS,0},{OP_CONST,3},{OP_MUL,0},
              {OP_CONST,1},{OP_SUB,0},{OP_SAT,0}})},
        };
        printf("---EMIT---\n");
        for(auto&e:tv){
            ExprProgram p=e.p; expr_validate(&p);
            for(int k=0;k<5;k++){
                ExprCtx cc=c; cc.x=k*0.25f; cc.t=0.7f*k;
                printf("%s|%d|%.7f\n", e.name, k, expr_eval(&p,&cc));
            }
        }
    }

    printf("vm: %d passed, %d failed\n", pass, fail);
    return fail?1:0;
}
