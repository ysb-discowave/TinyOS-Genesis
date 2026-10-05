#include <stdio.h>
#include <string.h>
#include "sshd_crypto.h"
static void pb(const char *tag, const c_u8 *b, int n){
  printf("%s ", tag); for(int i=0;i<n;i++) printf("%02x", b[i]); printf("\n");
}
static void fe_of(cx_fe h, int v){ cx_fe_zero(h); h[0] = v; }
int main(void){
  cx_ed_init();
  cx_fe t; c_u8 o[32];

  cx_fe_tobytes(o, cx_ed_d);  pb("D", o, 32);
  cx_fe_tobytes(o, cx_ed_d2); pb("D2", o, 32);

  cx_ed_pt B; cx_ed_base(&B);
  cx_fe_tobytes(o, B.X); pb("BX", o, 32);
  cx_fe_tobytes(o, B.Y); pb("BY", o, 32);
  { c_u8 e[32]; cx_ed_pt_encode(e, &B); pb("ENC_B", e, 32); }

  /* 基点是否在曲线上：-x^2 + y^2 = 1 + d x^2 y^2 */
  { cx_fe x2,y2,lhs,rhs,t1,t2;
    cx_fe_sq(x2, B.X); cx_fe_sq(y2, B.Y);
    cx_fe_sub(lhs, y2, x2);
    cx_fe_mul(t1, cx_ed_d, x2); cx_fe_mul(t2, t1, y2); cx_fe_add(t2, t2, (cx_fe){1,0,0,0,0,0,0,0,0,0});
    cx_fe_sub(rhs, t2, (cx_fe){0,0,0,0,0,0,0,0,0,0});
    cx_fe_copy(rhs, t2);
    cx_fe_sub(t, lhs, rhs);
    printf("ON_CURVE %s\n", cx_fe_iszero(t) ? "01" : "00");
  }

  cx_ed_pt P1, P2;
  cx_ed_pt_identity(&P1); cx_ed_pt_add(&P2, &P1, &B);
  { c_u8 e[32]; cx_ed_pt_encode(e, &P2); pb("1B", e, 32); }
  cx_ed_pt_dbl(&P2, &B);
  { c_u8 e[32]; cx_ed_pt_encode(e, &P2); pb("2B", e, 32); }
  { cx_ed_pt T; cx_ed_pt_add(&T, &P2, &B); c_u8 e[32]; cx_ed_pt_encode(e, &T); pb("3B", e, 32); }

  { c_u8 sc[32]; memset(sc,0,32); sc[0]=8;
    cx_ed_pt T; cx_ed_scalarmult(&T, sc, &B); c_u8 e[32]; cx_ed_pt_encode(e, &T); pb("8B", e, 32); }

  { /* L = 2^252 + 27742317777372353535851937790883648493 */
    c_u8 sc[32];
    static const c_u8 lv[32] = {0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
                                0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10};
    memcpy(sc, lv, 32);
    cx_ed_pt T; cx_ed_scalarmult(&T, sc, &B); c_u8 e[32]; cx_ed_pt_encode(e, &T); pb("LB", e, 32);
  }
  return 0;
}
